/*
** doombase.zs
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2006-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

extend struct _
{
	native readonly Array<class<Actor> > AllActorClasses;
	native readonly Array<@PlayerClass> PlayerClasses;
	native readonly Array<@PlayerSkin> PlayerSkins;
	native readonly Array<@Team> Teams;
	native readonly Array<@TerrainDef> Terrains;
	native int validcount;
	native play @DehInfo deh;
	native readonly ui bool automapactive;	// is automap enabled?
	native readonly ui bool viewactive;		// if automap is active, true = main automap, false = overlay automap.
	native readonly TextureID skyflatnum;
	native readonly int gametic;
	native readonly int Net_Arbitrator;
	native ui BaseStatusBar StatusBar;
	native readonly Weapon WP_NOCHANGE;
	deprecated("3.8", "Use Actor.isFrozen() or Level.isFrozen() instead") native readonly bool globalfreeze;
	native int LocalViewPitch;

	// sandbox state in multi-level setups:
	native play @PlayerInfo players[MAXPLAYERS];
	native readonly bool playeringame[MAXPLAYERS];
	native play LevelLocals Level;

	native readonly Array<@EpisodeInfo> AllEpisodes;
	native readonly Array<@SkillInfo> AllSkills;
}

struct EpisodeInfo native
{
	native readonly string mEpisodeName;
	native readonly string mEpisodeMap;
	native readonly string mPicName;
	native readonly int8 mShortcut;
	native readonly bool mNoSkill;
}

struct SkillInfo native
{
	native readonly Name SkillName;
	native readonly double AmmoFactor, DoubleAmmoFactor, DropAmmoFactor;
	native readonly double DamageFactor;
	native readonly double ArmorFactor;
	native readonly double HealthFactor;
	native readonly double KickbackFactor;

	native readonly bool FastMonsters;
	native readonly bool SlowMonsters;
	native readonly bool DisableCheats;
	native readonly bool AutoUseHealth;

	native readonly bool EasyBossBrain;
	native readonly bool EasyKey;
	native readonly bool NoMenu;
	native readonly int RespawnCounter;
	native readonly int RespawnLimit;
	native readonly double Aggressiveness;
	native readonly int SpawnFilter;
	native readonly bool SpawnMulti;
	native readonly bool InstantReaction;
	native readonly bool SpawnMultiCoopOnly;
	native readonly int ACSReturn;
	native readonly string MenuName;
	native readonly string PicName;
	native readonly Map<Name, string> MenuNamesForPlayerClass;
	native readonly bool MustConfirm;
	native readonly string MustConfirmText;
	native readonly int8 Shortcut;
	native readonly string TextColor;
	native readonly Map<Name, Name> Replace;
	native readonly Map<Name, Name> Replaced;
	native readonly double MonsterHealth;
	native readonly double FriendlyHealth;
	native readonly bool NoPain;
	native readonly int Infighting;
	native readonly bool PlayerRespawn;

	native int GetTextColor() const;
}

extend struct TexMan
{
	native static void SetCameraToTexture(Actor viewpoint, String texture, double fov);
	native static void SetCameraTextureAspectRatio(String texture, double aspectScale, bool useTextureRatio = true);
	native static void SetCanvasTextureTranslucent(String texture, bool translucent = true);
	deprecated("3.8", "Use Level.ReplaceTextures() instead") static void ReplaceTextures(String from, String to, int flags)
	{
		level.ReplaceTextures(from, to, flags);
	}
}

extend struct Screen
{
	native static void DrawFrame(int x, int y, int w, int h);
	// This is a leftover of the abandoned Inventory.DrawPowerup method.
	deprecated("2.5", "Use StatusBar.DrawTexture() instead") static ui void DrawHUDTexture(TextureID tex, double x, double y)
	{
		statusBar.DrawTexture(tex, (x, y), BaseStatusBar.DI_SCREEN_RIGHT_TOP, 1., (32, 32));
	}
}

extend struct Console
{
	native static void MidPrint(Font fontname, string textlabel, bool bold = false);
}

extend struct Translation
{
	native static bool SetPlayerTranslation(int group, int num, int plrnum, PlayerClass pclass);
}

// This is needed because Actor contains a field named 'translation' which shadows the above.
struct Translate version("4.5")
{
	static TranslationID MakeID(int group, int num)
	{
		return Translation.MakeID(group, num);
	}
	static bool SetPlayerTranslation(int group, int num, int plrnum, PlayerClass pclass)
	{
		return Translation.SetPlayerTranslation(group, num, plrnum, pclass);
	}
	static TranslationID GetID(Name transname)
	{
		return Translation.GetID(transname);
	}
}

struct DamageTypeDefinition native
{
	native static bool IgnoreArmor(Name type);
}

extend struct CVar
{
	native static CVar GetCVar(Name name, PlayerInfo player = null);
}

extend struct GameInfoStruct
{
	// will be extended as needed.
	native Name backpacktype;
	native double Armor2Percent;
	native String ArmorIcon1;
	native String ArmorIcon2;
	native Name BasicArmorClass;
	native Name HexenArmorClass;
	native bool norandomplayerclass;
	native Array<Name> infoPages;
	native GIFont mStatscreenMapNameFont;
	native GIFont mStatscreenEnteringFont;
	native GIFont mStatscreenFinishedFont;
	native GIFont mStatscreenContentFont;
	native GIFont mStatscreenAuthorFont;
	native double gibfactor;
	native bool intermissioncounter;
	native Color defaultbloodcolor;
	native double telefogheight;
	native int defKickback;
	native int defaultdropstyle;
	native TextureID healthpic;
	native TextureID berserkpic;
	native double normforwardmove[2];
	native double normsidemove[2];
	native bool mHideParTimes;
	native readonly double BloodSplatDecalDistance;
}

extend class Object
{
	private native static Object BuiltinNewDoom(Class<Object> cls, int outerclass, int compatibility);
	private native static TranslationID BuiltinFindTranslation(Name nm);
	private native static int BuiltinCallLineSpecial(int special, Actor activator, int arg1, int arg2, int arg3, int arg4, int arg5);
	private native static State BuiltinStateOffset(State st, int offset);
	// These really should be global functions...
	native static String G_SkillName();
	native static int G_SkillPropertyInt(int p);
	native static double G_SkillPropertyFloat(int p);
	deprecated("3.8", "Use Level.PickDeathMatchStart() instead") static vector3, int G_PickDeathmatchStart()
	{
		let [a,b] = level.PickDeathmatchStart();
		return a, b;
	}
	deprecated("3.8", "Use Level.PickPlayerStart() instead") static vector3, int G_PickPlayerStart(int pnum, int flags = 0)
	{
		let [a,b] = level.PickPlayerStart(pnum, flags);
		return a, b;
	}
	deprecated("4.3", "Use S_StartSound() instead") native static void S_Sound (Sound sound_id, int channel, float volume = 1, float attenuation = ATTN_NORM, float pitch = 0.0, float startTime = 0.0);
	native static void S_StartSound (Sound sound_id, int channel, int flags = 0, float volume = 1, float attenuation = ATTN_NORM, float pitch = 0.0, float startTime = 0.0);
	native static void S_StartSoundAt(Vector3 pos, Sound sound_id, int channel, int flags = 0, double volume = 1, double attenuation = ATTN_NORM, double pitch = 0.0, double startTime = 0.0);
	native static void S_PauseSound (bool notmusic, bool notsfx);
	native static void S_ResumeSound (bool notsfx);
	native static bool S_ChangeMusic(String music_name, int order = 0, bool looping = true, bool force = false);
	native static float S_GetLength(Sound sound_id);
	native static void MarkSound(Sound snd);
	native static uint BAM(double angle);
	native static void SetMusicVolume(float vol);
}

class Thinker : Object native play
{
	enum EStatnums
	{
		// Thinkers that don't actually think
		STAT_INFO,								// An info queue
		STAT_DECAL,								// A decal
		STAT_AUTODECAL,							// A decal that can be automatically deleted
		STAT_CORPSEPOINTER,						// An entry in Hexen's corpse queue
		STAT_TRAVELLING,						// An actor temporarily travelling to a new map
		STAT_STATIC,

		// Thinkers that do think
		STAT_FIRST_THINKING=32,
		STAT_SCROLLER=STAT_FIRST_THINKING,		// A DScroller thinker
		STAT_PLAYER,							// A player actor
		STAT_BOSSTARGET,						// A boss brain target
		STAT_LIGHTNING,							// The lightning thinker
		STAT_DECALTHINKER,						// An object that thinks for a decal
		STAT_INVENTORY,							// An inventory item
		STAT_LIGHT,								// A sector light effect
		STAT_LIGHTTRANSFER,						// A sector light transfer. These must be ticked after the light effects.
		STAT_EARTHQUAKE,						// Earthquake actors
		STAT_MAPMARKER,							// Map marker actors
		STAT_DLIGHT,							// Dynamic lights

		STAT_USER = 70,
		STAT_USER_MAX = 90,

		STAT_DEFAULT = 100,						// Thinkers go here unless specified otherwise.
		STAT_SECTOREFFECT,						// All sector effects that cause floor and ceiling movement
		STAT_ACTORMOVER,						// actor movers
		STAT_SCRIPTS,							// The ACS thinker. This is to ensure that it can't tick before all actors called PostBeginPlay
		STAT_BOT,								// Bot thinker
		MAX_STATNUM = 127
	}


	native LevelLocals Level;

	virtual native void Tick();
	virtual native void PostBeginPlay();
	virtual void OnLoad() {}
	native void AddToTravellingList();
	native void ChangeStatNum(int stat);
	native clearscope int GetStatNum() const;

	static clearscope int Tics2Seconds(int tics)
	{
		return int(tics / TICRATE);
	}

	//===========================================================================
	//
	// Called before the Thinker moves to another map, in case it needs to do
	// special clean-up.
	//
	//===========================================================================

	virtual void PreTravelled() {}

	//===========================================================================
	//
	// Called after the Thinker moved to another map, in case it needs to do
	// special reinitialization.
	//
	//===========================================================================

	virtual void Travelled() {}

}

class ThinkerIterator : Object native
{
	native static ThinkerIterator Create(class<Object> type = "Actor", int statnum=Thinker.MAX_STATNUM+1, bool clientSide = false);
	native Thinker Next(bool exact = false);
	native void Reinit();
}

class ActorIterator : Object native
{
	deprecated("3.8", "Use Level.CreateActorIterator() instead") static ActorIterator Create(int tid, class<Actor> type = "Actor")
	{
		return level.CreateActorIterator(tid, type);
	}
	native Actor Next();
	native void Reinit();
}

class BlockThingsIterator : Object native
{
	native Actor thing;
	native Vector3 position;
	native int portalflags;

	native static BlockThingsIterator Create(Actor origin, double checkradius = -1, bool ignorerestricted = false);
	native static BlockThingsIterator CreateFromPos(double checkx, double checky, double checkz, double checkh, double checkradius, bool ignorerestricted);
	native bool Next();
}

class BlockLinesIterator : Object native
{
	native Line CurLine;
	native Vector3 position;
	native int portalflags;

	native static BlockLinesIterator Create(Actor origin, double checkradius = -1);
	native static BlockLinesIterator CreateFromPos(Vector3 pos, double checkh, double checkradius, Sector sec = null);
	native bool Next();
}

enum ETraceStatus
{
	TRACE_Stop,		// stop the trace, returning this hit
	TRACE_Continue,		// continue the trace, returning this hit if there are none further along
	TRACE_Skip,		// continue the trace; do not return this hit
	TRACE_Abort		// stop the trace, returning no hits
}

enum ETraceFlags
{
	TRACE_NoSky		= 0x0001,	// Hitting the sky returns TRACE_HitNone
	//TRACE_PCross		= 0x0002,	// Trigger SPAC_PCROSS lines
	//TRACE_Impact		= 0x0004,	// Trigger SPAC_IMPACT lines
	TRACE_PortalRestrict	= 0x0008,	// Cannot go through portals without a static link offset.
	TRACE_ReportPortals	= 0x0010,	// Report any portal crossing to the TraceCallback
	//TRACE_3DCallback	= 0x0020,	// [ZZ] use TraceCallback to determine whether we need to go through a line to do 3D floor check, or not. without this, only line flag mask is used
	TRACE_HitSky		= 0x0040	// Hitting the sky returns TRACE_HasHitSky
}


enum ETraceResult
{
	TRACE_HitNone,
	TRACE_HitFloor,
	TRACE_HitCeiling,
	TRACE_HitWall,
	TRACE_HitActor,
	TRACE_CrossingPortal,
	TRACE_HasHitSky
}

enum ELineTier
{
	TIER_Middle,
	TIER_Upper,
	TIER_Lower,
	TIER_FFloor
}

struct TraceResults native
{
	native Sector HitSector; // originally called "Sector". cannot be named like this in ZScript.
	native TextureID HitTexture;
	native vector3 HitPos;
	native vector3 HitVector;
	native vector3 SrcFromTarget;
	native double SrcAngleFromTarget;

	native double Distance;
	native double Fraction;

	native Actor HitActor;		// valid if hit an actor. // originally called "Actor".

	native Line HitLine;		// valid if hit a line // originally called "Line".
	native uint8 Side;
	native uint8 Tier;		// see Tracer.ELineTier
	native bool unlinked;		// passed through a portal without static offset.

	native ETraceResult HitType;
	native F3DFloor ffloor;

	native Sector CrossedWater;		// For Boom-style, Transfer_Heights-based deep water
	native vector3 CrossedWaterPos;	// remember the position so that we can use it for spawning the splash
	native F3DFloor Crossed3DWater;	// For 3D floor-based deep water
	native vector3 Crossed3DWaterPos;
}

class LineTracer : Object native
{
	native @TraceResults Results;
	native bool Trace(vector3 start, Sector sec, vector3 direction, double maxDist, ETraceFlags traceFlags, /* Line::ELineFlags */ uint wallMask = 0xFFFFFFFF, bool ignoreAllActors = false, Actor ignore = null);

	virtual ETraceStatus TraceCallback()
	{
		// Normally you would examine Results.HitType (for ETraceResult), and determine either:
		//  - stop tracing and return the entity that was found (return TRACE_Stop)
		//  - ignore some object, like noclip, e.g. only count solid walls and floors, and ignore actors (return TRACE_Skip)
		//  - find last object of some type (return TRACE_Continue)
		//  - stop tracing entirely and assume we found nothing (return TRACE_Abort)
		// TRACE_Abort and TRACE_Continue are of limited use in scripting.

		return TRACE_Stop; // default callback returns first hit, whatever it is.
	}
}

struct DropItem native
{
	native readonly DropItem Next;
	native readonly name Name;
	native readonly int Probability;
	native readonly int Amount;
}

struct LevelInfo native
{
	native readonly int levelnum;
	native readonly String MapName;
	native readonly String NextMap;
	native readonly String NextSecretMap;
	native readonly String SkyPic1;
	native readonly String SkyPic2;
	native readonly String SkyMistPic;
	native readonly String F1Pic;
	native readonly int cluster;
	native readonly int partime;
	native readonly int sucktime;
	native readonly int flags;
	native readonly int flags2;
	native readonly int flags3;
	native readonly String LightningSound;
	native readonly String Music;
	native readonly String LevelName;
	native readonly String MapLabel;
	native readonly String AuthorName;
	native readonly int musicorder;
	native readonly float skyspeed1;
	native readonly float skyspeed2;
	native readonly float skymistspeed;
	native readonly float skymistyscale;
	native readonly int cdtrack;
	native readonly double gravity;
	native readonly double aircontrol;
	native readonly int airsupply;
	native readonly int compatflags;
	native readonly int compatflags2;
	native readonly name deathsequence;
	native readonly int fogdensity;
	native readonly int outsidefogdensity;
	native readonly int skyfog;
	native readonly float thickfogdistance;
	native readonly float thickfogmultiplier;
	native readonly float pixelstretch;
	native readonly name RedirectType;
	native readonly String RedirectMapName;
	native readonly double teamdamage;

	native bool isValid() const;
	native String LookupLevelName() const;

	native static int GetLevelInfoCount();
	native static LevelInfo GetLevelInfo(int index);
	native static LevelInfo FindLevelInfo(String mapname);
	native static LevelInfo FindLevelByNum(int num);
	native static bool MapExists(String mapname);
	native static String MapChecksum(String mapname);
}

struct FSpawnParticleParams
{
	native Color color1;
	native TextureID texture;
	native int style;
	native int flags;
	native int lifetime;

	native double size;
	native double sizestep;

	native Vector3 pos;
	native Vector3 vel;
	native Vector3 accel;

	native double startalpha;
	native double fadestep;
	native double fadeoutstep; // unlike fadestep, this is always expected to be a positive value.

	native double startroll;
	native double rollvel;
	native double rollacc;
};

struct LevelLocals native
{
	enum EUDMF
	{
		UDMF_Line,
		UDMF_Side,
		UDMF_Sector,
		//UDMF_Thing // not implemented
	};

	const CLUSTER_HUB = 0x00000001;	// Cluster uses hub behavior


	native Array<@Sector> Sectors;
	native Array<@Line> Lines;
	native Array<@Side> Sides;
	native readonly Array<@Vertex> Vertexes;
	native readonly Array<@LinePortal> LinePortals;
	native internal readonly Array<@SectorPortal> SectorPortals;

	native readonly int time;
	native readonly int maptime;
	native readonly int totaltime;
	native readonly int starttime;
	native readonly int partime;
	native readonly int sucktime;
	native readonly int cluster;
	native readonly int clusterflags;
	native readonly int levelnum;
	native readonly String LevelName;
	native readonly String MapName;
	native String NextMap;
	native String NextSecretMap;
	native readonly String F1Pic;
	native readonly int maptype;
	native readonly String AuthorName;
	native String LightningSound;
	native readonly String Music;
	native readonly int musicorder;
	native readonly TextureID skytexture1;
	native readonly TextureID skytexture2;
	native readonly TextureID skymisttexture;
	native float skyspeed1;
	native float skyspeed2;
	native float skymistspeed;
	native float skymistyscale;
	native int total_secrets;
	native int found_secrets;
	native int total_items;
	native int found_items;
	native int total_monsters;
	native int killed_monsters;
	native play double gravity;
	native play double aircontrol;
	native play double airfriction;
	native play int airsupply;
	native readonly double teamdamage;
	native readonly bool noinventorybar;
	native readonly bool monsterstelefrag;
	native readonly bool actownspecial;
	native readonly bool sndseqtotalctrl;
	native bool allmap;
	native readonly bool missilesactivateimpact;
	native readonly bool monsterfallingdamage;
	native readonly bool checkswitchrange;
	native readonly bool polygrind;
	native readonly bool nomonsters;
	native readonly bool allowrespawn;
	deprecated("3.8", "Use Level.isFrozen() instead") native bool frozen;
	native readonly bool infinite_flight;
	native readonly bool no_dlg_freeze;
	native readonly bool keepfullinventory;
	native readonly bool removeitems;
	native readonly bool useplayerstartz;
	native readonly int fogdensity;
	native readonly int outsidefogdensity;
	native readonly int skyfog;
	native readonly float thickfogdistance;
	native readonly float thickfogmultiplier;
	native readonly float pixelstretch;
	native readonly float MusicVolume;
	native name deathsequence;
	native readonly int compatflags;
	native readonly int compatflags2;
	native readonly LevelInfo info;

	native String GetUDMFString(int type, int index, Name key);
	native int GetUDMFInt(int type, int index, Name key);
	native double GetUDMFFloat(int type, int index, Name key);
	native play int ExecuteSpecial(int special, Actor activator, line linedef, bool lineside, int arg1 = 0, int arg2 = 0, int arg3 = 0, int arg4 = 0, int arg5 = 0);
	native void GiveSecret(Actor activator, bool printmsg = true, bool playsound = true);
	native void StartSlideshow(Name whichone = 'none');
	native static void MakeScreenShot();
	native static void MakeAutoSave();
	native void WorldDone();
	deprecated("3.8", "This function does nothing") static void RemoveAllBots(bool fromlist) { /* intentionally left as no-op. */ }
	native ui Vector2 GetAutomapPosition();
	native void SetInterMusic(String nextmap);
	native String FormatMapName(int mapnamecolor);
	native bool IsJumpingAllowed() const;
	native bool IsCrouchingAllowed() const;
	native bool IsFreelookAllowed() const;
	native void StartIntermission(Name type, int state) const;
	native play SpotState GetSpotState(bool create = true);
	native clearscope int FindUniqueTid(int start = 0, int limit = 0, bool clientSide = false);
	native uint GetSkyboxPortal(Actor actor);
	native void ReplaceTextures(String from, String to, int flags);
	clearscope native HealthGroup FindHealthGroup(int id);
	native vector3, int PickDeathmatchStart();
	native vector3, int PickPlayerStart(int pnum, int flags = 0);
	native int isFrozen() const;
	native void setFrozen(bool on);
	native string LookupString(uint index);

	native clearscope Sector PointInSector(Vector2 pt) const;

	native clearscope bool IsPointInLevel(vector3 p) const;
	deprecated("3.8", "Use Level.IsPointInLevel() instead") clearscope static bool IsPointInMap(vector3 p)
	{
		return level.IsPointInLevel(p);
	}

	native clearscope vector2 Vec2Diff(vector2 v1, vector2 v2) const;
	native clearscope vector3 Vec3Diff(vector3 v1, vector3 v2) const;
	native clearscope vector3 SphericalCoords(vector3 viewpoint, vector3 targetPos, vector2 viewAngles = (0, 0), bool absolute = false) const;

	native clearscope vector2 Vec2Offset(vector2 pos, vector2 dir, bool absolute = false) const;
	native clearscope vector3 Vec2OffsetZ(vector2 pos, vector2 dir, double atz, bool absolute = false) const;
	native clearscope vector3 Vec3Offset(vector3 pos, vector3 dir, bool absolute = false) const;
	native clearscope Vector2 GetDisplacement(int pg1, int pg2) const;
	native clearscope int GetPortalGroupCount() const;
	native clearscope int PointOnLineSide(Vector2 pos, Line l, bool precise = false) const;
	native clearscope int ActorOnLineSide(Actor mo, Line l) const;
	native clearscope int BoxOnLineSide(Vector2 pos, double radius, Line l) const;

	native clearscope int PlayerNum(PlayerInfo player) const;

	// [BB] Names for the billboard payloads, facing modes and flags below.
	// These mirror EBillboardPayload / EBillboardFacing / EBillboardFlags in
	// g_levellocals.h. They live here so callers are not obliged to invent
	// their own copies of the same numbers, which is how two sets of
	// constants for one thing start drifting apart.
	enum EBillboardPayload
	{
		BB_PANEL   = 0,		// rounded-rect backing
		BB_TEXTURE = 1,		// any texture; data = TextureID.GetIndex()
		BB_DIGITS  = 2,		// one integer, printed; data = the value
		BB_GLYPH   = 3,		// one character; data = its code
		BB_RING    = 4,
		BB_BAR     = 5,
		// Arbitrary text -- pass the `text` argument, not `data`. BB_DIGITS
		// can only ever show a number because an int is all `data` is; this
		// takes a string, so names, IDs and labels fit.
		BB_TEXT    = 6,
		// The same string drawn as a 16-segment display -- the arcade
		// readout look. No atlas: the glyphs are built from arithmetic in
		// the shader, so this cannot be broken by a missing font lump and
		// stays perfectly sharp at any size. Numbers want this; names want
		// BB_TEXT. Draws a bordered plate behind itself.
		BB_SEGMENT = 7,
		// The same display with its polarity inverted: a lit face with the
		// characters punched out of it dark, which is what GITD's original
		// did and what an LCD looks like. BB_SEGMENT is the LED version --
		// dark bed, glowing segments. Pick by which reads better against the
		// room, not by which is more correct.
		BB_SEGLCD  = 8,
		// A glowing slit. Open it by animating its WIDTH with
		// ResizeBillboard -- the shader deliberately has no progress term, so
		// the easing, the pause and the reverse all belong to the caller.
		// Flat (tilt 90) it is a seam in the floor; upright (tilt 0) it is a
		// door something can walk out of.
		BB_SEAM    = 9,
		// GITD's kill badge, transcribed from the original shader rather than
		// rebuilt. A lozenge plate with the number punched out of it in
		// black, all in one pass. `data` is the number. Drive progress to
		// open it -- a thin slit at 0, full lozenge at 1, digits appearing
		// past 0.55. Digits only; letters are BB_SEGMENT.
		//
		// Size it as the original does: halfH 46, and
		// halfW = halfH * (0.60 + digits * 0.42). Square dimensions give a
		// circle, which is not what it is meant to be.
		BB_WG13    = 10,
		// BB_PANEL's job as a distance field rather than a sampled texture.
		// Crisp at any size, and -- the reason it exists -- it can take
		// SetBillboardGlow, which a sampled plate structurally cannot: a halo
		// needs a field to read past the shape's edge and a texture has none.
		//
		// Both exist and the caller picks. Sampling one small texture is
		// cheaper than solving two distance fields, so a ring of background
		// plates nobody looks closely at should stay BB_PANEL.
		//
		// data packs the shape: byte 0 = corner radius 0-15, byte 1 = border
		// width 0-15, each across the half-extent. Border 0 is a plain plate.
		// BBFL_VOID turns it into a hole -- dark inside, only the rim lit --
		// exactly as it does on BB_SEAM.
		BB_SDFPANEL = 11,
		// A HEXAGON, for tessellation. Same field, same halo, same border and the
		// same two shape nibbles as BB_SDFPANEL -- only the distance function is
		// different. Its own payload because a comb SHARES EDGES, and a shared
		// edge is the one place a sampled shape cannot hide: two neighbours each
		// half a pixel soft do not meet, they seam. Pointy-top.
		BB_SDFHEX  = 12,

		// AN N-POINTED STAR POLYGON -- the symbol a chart draws, filled or
		// stroked as an outline. A glyph rather than a point of light, because
		// a glyph has an inside to name. shape byte0 = stroke width 0-15 where
		// 0 means FILLED, byte1 = point count (under 3 means 5). The shape sits
		// well inside its quad so the halo has room to fall off; size the
		// billboard about 1.7x the star you want to see.
		BB_SDFSTAR = 13,
	}

	enum EBillboardFacing
	{
		BBF_FIXED     = 0,	// use my own yaw and tilt
		BBF_CAMERAYAW = 1,	// turn to the viewer, stay upright
		BBF_CAMERA    = 2,	// turn to the viewer including tilt
	}

	enum EBillboardFlags
	{
		BBFL_PERSISTENT  = 1,
		BBFL_ATTACHED    = 2,
		BBFL_NODEPTH     = 4,
		BBFL_VIEWLOCKED  = 8,
		BBFL_FOLLOWANGLE = 16,
		// Decoration: drawn, but never returned by AimBillboard,
		// TouchBillboard or SweepBillboard. A composed panel is forty small
		// billboards -- every glyph of every label, a bar's track and its
		// fill -- and the queries return the NEAREST hit, so without this the
		// panel's own face is permanently masked by the text written on it
		// and a pointer aimed at a row comes back holding a letter.
		BBFL_NOHIT       = 32,
		// BB_SEAM only: the opening is a HOLE -- dark inside, bright rim --
		// instead of a lit panel. Use it whenever something is supposed to
		// come OUT of the seam, or it reads as standing in front of a light.
		BBFL_VOID        = 64,
	}

	// [BB] Billboards -- world-anchored oriented quads, the native backing
	// for in-world panels. Extent is per-axis and orientation is explicit,
	// because a quad that always turns to the camera cannot be hinged to
	// another at a fixed angle: once both turn independently the angle
	// between them stops meaning anything. Hinge solving stays in script;
	// pass the yaw/tilt you already worked out.
	//
	// yaw  = which way the face points
	// tilt = 0 is vertical, positive leans the top toward the viewer
	// facing: 0 = use my own orientation, 1 = turn to viewer but stay
	//         upright, 2 = turn to viewer including tilt
	// payload: 0 = panel, 1 = texture (data = TextureID.GetIndex()),
	//          2 = digits, 3 = glyph, 4 = ring, 5 = bar,
	//          6 = text (uses the `text` argument, ignores data)
	// flags: 1 = persistent, 2 = attached, 4 = no depth test,
	//        8 = view-locked (pos becomes an offset from the viewer:
	//            X ahead, Y right, Z up). Resolved at render rate, so a
	//            HUD-locked panel stays welded to the view instead of
	//            lagging and snapping the way a script-moved one would.
	//       16 = follow angle (attached only: yaw becomes relative to the
	//            actor's facing, so faces turn with it)
	//
	// `text` is BB_TEXT's payload and is ignored by every other one. It is a
	// trailing default so no existing caller has to change.
	//
	// Transient -- expires by lifetime, no handle issued.
	native void AddBillboard(Vector3 pos, double w, double h, double yaw, double tilt, int facing, int payload, int data, color col, int flags = 0, double lifetime = 0, string text = "");
	// Persistent -- lives until RemoveBillboard(id). Returns a handle.
	native int AddBillboardPersistent(Vector3 pos, double w, double h, double yaw, double tilt, int facing, int payload, int data, color col, int flags = 0, double lifetime = 0, string text = "");
	// Attached -- follows mo at offset and dies with it. Returns a handle.
	native int AttachBillboard(Actor mo, Vector3 offset, double w, double h, double yaw, double tilt, int facing, int payload, int data, color col, int flags = 0, string text = "");
	native void UpdateBillboard(int id, int data, color col);
	// Retext a live BB_TEXT billboard. Separate from UpdateBillboard for the
	// same reason SetBillboardAlpha is: a readout that restrings itself every
	// tic should not have to restate its colour to do it.
	native void SetBillboardText(int id, string text);
	// Neon, for BB_TEXT. radius is a fraction of the atlas spread -- 1.0 uses
	// the whole field and is the practical maximum, because past the spread
	// there is nothing left to read and the halo clips to a hard square at the
	// glyph's cell edge. strength 0 is off, 1 is a halo as bright as the core.
	native void SetBillboardGlow(int id, double radius, double strength);
	// The far end of a gradient. Alpha 0 switches it off; a payload that
	// supports gradients then draws `col` flat. Its own setter because the
	// Add functions are already near the argument-count cliff.
	native void SetBillboardGradient(int id, color col2);
	// How far through its reveal, 0..1. On BB_SEGMENT / BB_SEGLCD the plate is
	// a thin slit at 0 that opens vertically into a full ellipse, and the
	// characters only appear past 0.55 -- which is the reveal GITD's original
	// wgType 13 had and the reason it was worth stealing. 1 = fully formed.
	native void SetBillboardProgress(int id, double t);
	// How wide a BB_TEXT string will draw at this height, in map units.
	// Returns 0 if no SDF atlas is loaded -- treat that as "estimate it
	// yourself", not as an empty string. Beats counting characters, which is
	// only right for a monospace atlas and breaks silently on any other.
	// Which typeface a billboard draws in. 0 is the default face; 1 through
	// BillboardFontCount() index the roster, which is RESHUFFLED EVERY GAME --
	// so ask for a slot because you want "the display face", never because
	// you want a particular font. Out of range draws in the default face.
	// A setter and not an argument: AddBillboardPersistent is already at
	// fourteen natively and sixteen crashes the script compiler outright.
	native void SetBillboardFont(int id, int slot);
	// Reshuffle. Call at game start for a fresh look; no atlas is reloaded.
	native void RollBillboardFonts();
	// Rolled faces available, NOT counting slot 0. Zero is a legitimate load
	// -- it means only the default face shipped -- so read it as "do not
	// bother varying the typeface", not as a failure.
	native int BillboardFontCount();
	// Which face is in that slot right now. Changes on every roll, which is
	// why anything wanting to name it has to ask rather than remember.
	native string BillboardFontName(int slot);

	// A string carrying '\n' draws as stacked, centred lines, so `height` is
	// the height of ONE line and this reports the WIDEST one.
	//
	// fontSlot MUST be the slot the billboard will draw in. Faces have
	// different advances, so measuring in one and drawing in another is
	// silently wrong -- and with a reshuffled roster it would be wrong by a
	// different amount every game.
	native double MeasureBillboardText(string text, double height, int fontSlot = 0);
	// Width AND total height of a multi-line BB_TEXT string, for the given
	// per-line height. Returned as a pair because the line pitch is the
	// renderer's rule and script cannot derive it -- ask, do not multiply by a
	// constant of your own, or your layout goes stale the day the pitch moves.
	// Single-line text returns exactly (width, height), so this is always safe.
	native Vector2 MeasureBillboardTextBlock(string text, double height, int fontSlot = 0);

	// Pack a halo into the `data` argument of a BB_TEXT billboard. BB_TEXT
	// ignores data otherwise, so this costs nothing and needs no extra
	// parameter -- which matters, because adding two more would put
	// AddBillboardPersistent at sixteen arguments and the script compiler
	// CRASHES compiling a call to a native with that many. No error, no
	// dialog, just a silent exit while loading actors.
	//
	//   level.AddBillboard(pos, w, h, yaw, tilt, facing, LevelLocals.BB_TEXT,
	//                      LevelLocals.BBGlow(0.5, 0.75), col, 0, 2, "ELITE");
	static int BBGlow(double radius, double strength)
	{
		int r = int(clamp(radius, 0.0, 1.0) * 255.0 + 0.5);
		int s = int(clamp(strength, 0.0, 1.0) * 255.0 + 0.5);
		return r | (s << 8);
	}
	native void MoveBillboard(int id, Vector3 pos);
	native void OrientBillboard(int id, double yaw, double tilt, int facing);
	// Spin in the quad's own plane, degrees. Yaw and tilt aim a billboard;
	// roll turns its face. Independent of facing, so a camera-facing panel
	// still rolls -- the camera solve decides where the face points, roll
	// decides which way is up on it.
	//
	// Lives in the SHARED basis, so a rolled billboard is pointable exactly
	// where it is drawn: AimBillboard, TouchBillboard and SweepBillboard all
	// see the rotation. Its own setter rather than a fourth argument to
	// OrientBillboard, which is called every tic by everything.
	native void RollBillboard(int id, double roll);
	native void ResizeBillboard(int id, double w, double h);
	// 0 = invisible, 1 = opaque. Separate from UpdateBillboard because a fade
	// runs every tic while data and colour rarely change.
	native void SetBillboardAlpha(int id, double alpha);
	native void RemoveBillboard(int id);

	// [BB] GROUPS -- one transform over a whole composed panel.
	//
	// A panel is forty quads. Scaling it means scaling each one's size AND
	// its offset from the panel's centre; doing that from here would be
	// eighty setter calls per step and, worse, it would STEP -- script runs
	// at 35Hz and the renderer does not. Declare the animation once and the
	// engine resolves it per frame:
	//
	//     int gid = level.AddBillboardGroup((AHEAD, 0, UP));
	//     ... build, calling level.SetBillboardGroup(id, gid) on each element
	//     level.AnimateBillboardGroup(gid, 0.0, 1.0, 10);   // and that is all
	//
	// The origin is in the MEMBERS' OWN space: an offset from the viewer for
	// BBFL_VIEWLOCKED, from the actor for BBFL_ATTACHED, a world point
	// otherwise. Do not mix spaces inside one group.
	//
	// Growth eases out with a slight overshoot, collapse eases in; the curve
	// is the engine's and is not a parameter. Scale 0 draws nothing at all,
	// so a group is also how you hide a panel without destroying it.
	native int  AddBillboardGroup(Vector3 origin);
	native void SetBillboardGroup(int id, int gid);
	native void SetBillboardGroupScale(int gid, double scale);
	native void AnimateBillboardGroup(int gid, double from, double to, int tics);
	native void SetBillboardGroupOrigin(int gid, Vector3 origin);
	// Releases every member back to untransformed. Members left pointing at a
	// dead group would silently snap to full size, so this is not optional
	// cleanup -- it is the correct way to end a group's life.
	native void RemoveBillboardGroup(int gid);
	// Ray versus billboard. Returns the nearest one the ray crosses and where
	// on its face it landed, as 0..1 across and down -- the same UV the shader
	// sees. Returns 0 on a miss. maxDist <= 0 means unlimited. Call as:
	//   int hit; Vector2 uv; [hit, uv] = level.AimBillboard(start, dir);
	// [BB] Claim the VR sticks while a script-side selector is open.
	//
	// Snap turn and stick movement are decided deep in the VR input path, before
	// any script sees a button -- so without this a mod whose menu is driven by
	// the thumbstick spins and walks the player while they are choosing, which
	// is the most disorienting thing an in-world menu can do.
	//
	// Set true on open, false on close, and clear it on level end or death too:
	// nothing else will, and a stuck flag is a player who cannot turn.
	native void SuppressVRInput(bool suppressed);

	// [BB] The locomotion stick's raw deflection -- (forward, side) -- read
	// straight from the VR input path, NOT gated by SuppressVRInput above.
	// That call zeroes the one other channel script has for stick
	// deflection (cmd.sidemove/forwardmove) at the exact same source, so a
	// mod that suppresses the stick to stop it walking/turning the player
	// had no way to also read which way it's being pushed. This is that way
	// out: call it after SuppressVRInput(true) and it still reports the
	// real value.
	native Vector2 GetRawStickMove();

	// Force the laser sight on for as long as a script-side menu is up. An
	// override, not a settings change -- the archived cvars are untouched and the
	// player gets their own preference back the moment it is dropped.
	//
	// hand: -1 both, 0 main, 1 off. Name the hand wearing the menu -- the other
	// hand keeps whatever the player's own settings give it. The forced hand also
	// ignores the empty-hand and melee gates, so a menu on a bare off hand still
	// gets a cursor.
	native void ForceVRLaser(bool on, int hand = -1);

	// Stop the laser at a distance only script knows -- the engine trace cannot
	// see billboards, so without this the beam passes through the panel it is
	// selecting. Shortening only; 0 hands the decision back to the engine.
	// Applies to the hand named in ForceVRLaser, so an ordinary laser sight on
	// the other hand is not cut short by a menu it has nothing to do with.
	native void SetVRLaserRange(double range);

	// Buzz a controller. hand is 0 main, 1 off -- the abstract hand, not a
	// physical side; the handedness swap happens engine-side. intensity 0..1,
	// duration in milliseconds, both clamped. Honours vr_enable_haptics.
	native void VRHaptic(int hand, double intensity, double durationMs);

	// Named JSON profiles -- a flat key/double document per name, saved under
	// the same directory doomxr.ini itself lives in (a "rs_profiles"
	// subfolder), read and written through the engine's own JSON library so a
	// hand-edited file gets real parsing, not a hand-rolled ZScript scanner.
	//
	// PROTOCOL: exactly one profile is ever "in progress" at a time.
	//   SAVE:  JSONProfileBegin() -> JSONProfileSetDouble(key, val) per field
	//          -> JSONProfileSave(name)
	//   LOAD:  JSONProfileLoad(name) -> JSONProfileGetDouble(key, default) per
	//          field
	// name is restricted to [A-Za-z0-9_-], 1-64 characters -- anything else
	// is refused (false/no write/no load) rather than silently mangled, since
	// this writes to a real path on disk.
	native void JSONProfileBegin();
	native void JSONProfileSetDouble(string key, double value);
	native double JSONProfileGetDouble(string key, double defaultValue = 0.0);
	native bool JSONProfileSave(string name);
	native bool JSONProfileLoad(string name);

	native bool IsVRInputSuppressed();

	// FIELD REFLECTION -- read another mod's data without linking to it.
	//
	// A typed reference needs its class at COMPILE time, so describing another
	// mod's weapon normally means hard-depending on it. These take the field
	// name as a STRING and resolve at runtime, so a consumer carries no
	// reference to anything and still builds when that mod is absent. Service
	// (service.zs) solves the case where the other mod cooperates; this one
	// covers everything already released, which never will.
	//
	// Each returns FALSE when the field is missing, the wrong type, or not
	// readable, and leaves its out parameter untouched. False means "could not
	// answer", never "the answer is zero" -- a caller must be able to tell an
	// absent value from one that is genuinely 0.
	//
	// Private, meta and static fields are refused. Read-only fields are not:
	// read-only means "do not write", and none of these write. Inherited
	// fields resolve, so a subclass reports Weapon's own members too.
	//
	// Reads only, by design. A setter would put another mod's corruption and
	// its crash in different places with nothing tying them together.
	native bool HasField(Object o, string field);
	native bool GetFieldInt(Object o, string field, out int value);
	// GetFieldInt refuses an array field outright -- its Type is a PArray,
	// not TypeSInt32/TypeUInt32, so the scalar getter's own type check
	// correctly says no. This is the array-element counterpart: same
	// reflection, same read-only guarantee, plus an index bounds-checked
	// against the array's own declared size.
	native bool GetFieldIntArray(Object o, string field, int index, out int value);
	// Separate from GetFieldInt on purpose. Handles both storage shapes: a
	// standalone bool field, and a flagdef packed as a bit -- which is every
	// +WEAPON.OFFHANDWEAPON and every actor flag, readable here by name.
	native bool GetFieldBool(Object o, string field, out int value);
	// Widens: serves float32, float64 and integer fields, because every one of
	// those survives the conversion. Nothing narrows anywhere.
	native bool GetFieldFloat(Object o, string field, out double value);
	native bool GetFieldString(Object o, string field, out string value);
	native bool GetFieldName(Object o, string field, out name value);
	native bool GetFieldObject(Object o, string field, out Object value);

	// Enumeration -- what lets a consumer DISCOVER what a weapon carries
	// instead of only asking questions it already knew to ask. type comes back
	// as "int", "double", "string", "name", "object" or "other", so a caller
	// picks its getter without the engine exporting the type system.
	//
	// Costs a symbol lookup per call. Fine per selection change; cache it
	// rather than walking forty weapons every tic.
	native int  FieldCount(Object o);
	native bool FieldAt(Object o, int index, out string fieldName, out string fieldType);

	// What FindModelFrameRaw would resolve for (cls, sprite, frame), the part
	// no script could otherwise see: whether that MODELDEF block mirrors the
	// mesh (negative X Scale) and its baked AngleOffset/PitchOffset/RollOffset.
	// Mirroring is a per-model authoring choice with no relationship to which
	// hand a weapon is normally held in, so there was no way to compensate a
	// world-placed copy of a weapon's model correctly without this -- a single
	// guessed "flip 180" can only ever be right for some of the arsenal.
	// Returns false (all outputs 0) if the class has no model, or no
	// FSpriteModelFrame exists for that exact (sprite, frame).
	native bool, bool, double, double, double GetModelOrientationHint(class<Actor> cls, int sprite, int frame);

	// The model's baked POSITION offset (MODELDEF Offset/ZOffset), already
	// divided by that model's own scale -- ready to use directly as a
	// Doom-space local (X, Y, Z), no further conversion needed. A different
	// bug from GetModelOrientationHint: that one is rotation (why a stored
	// weapon pointed the wrong way), this one is displacement (why it did not
	// sit where the actor origin was placed). pixelstretch should be
	// level.info.pixelstretch, or 1.0 if unknown.
	native bool, double, double, double GetModelOffsetHint(class<Actor> cls, int sprite, int frame, double pixelstretch);

	// What GetModelOffsetHint's local offset becomes in WORLD space once
	// rotated by (actorAngle, actorPitch, actorRoll) -- computed by building
	// the SAME VSMatrix with the SAME rotate() calls RenderModel itself uses
	// (r_data/models.cpp), not by script re-deriving that rotation by hand.
	// That hand-derivation is exactly what went wrong building the holster
	// system's centering fix: two independent trig reconstructions, each
	// internally consistent, both wrong against the real renderer, because
	// neither accounted for RenderModel silently negating pitch before
	// rotating. This does not re-derive anything -- it replays the engine's
	// own transform on the offset directly.
	//
	// Returns the displacement a script needs to place an ACTOR at (i.e. the
	// actor's origin should be targetWorldPos - result) so that, once the
	// engine applies the model's own baked offset AND the actor's own
	// rotation on top, the MESH lands at targetWorldPos.
	// actorScaleX/Y are REQUIRED for a correct answer, not a refinement: the
	// renderer applies the model's baked offset BEFORE its scale (the offset
	// translate is issued after the scale call, and later matrix calls reach
	// the vertex first), so the actor's own Scale multiplies straight into the
	// displacement. Pass the same Scale the actor is drawn at -- passing 1,1
	// for a shrunken prop over-corrects by 1/scale and throws the mesh well
	// clear of where it was meant to sit.
	native bool, double, double, double GetModelWorldOffset(class<Actor> cls, int sprite, int frame,
		double pixelstretch, double actorAngle, double actorPitch, double actorRoll,
		double actorScaleX = 1.0, double actorScaleY = 1.0);

	// How physically big the model is, in world units, at actor Scale (1,1)
	// -- the measurement GetModelOrientationHint/OffsetHint/WorldOffset don't
	// give: they answer which way and how far off-center, not how large.
	// Solve scale = targetRadius / this per weapon instead of one flat
	// multiplier applied to every model regardless of its real size. A
	// conservative bounding-sphere radius (largest |X|/|Y|/|Z| independently,
	// not necessarily one vertex), not a tight fit -- errs toward showing
	// slightly small rather than clipping outside a holster's marker.
	// found=0 for the usual reasons (no model, no matching frame) plus any
	// model format that has not measured its own geometry for this.
	native bool, double GetModelBoundsHint(class<Actor> cls, int sprite, int frame);

	// Which class's MODELDEF a live ACTOR INSTANCE actually resolves
	// against right now -- modelData->modelDef if something (A_ChangeModel,
	// called on this exact instance) set a per-instance override, else the
	// actor's own class. Lets a holster/prop system show the correct model
	// for a weapon another mod has model-swapped per-instance (ModelSwapper
	// is the motivating case) instead of doing its own class-name lookup,
	// which finds nothing for a weapon that was never given its own
	// MODELDEF entry -- the swap only ever lived on the instance.
	native class<Actor> GetActorModelClass(Actor act);

	native int, Vector2 AimBillboard(Vector3 start, Vector3 dir, double maxDist = 0);
	// Point versus billboard -- the touch case. Returns the nearest billboard
	// the point is within maxRange of and inside the bounds of, its UV, and
	// the distance to the surface. Drive hover off the distance and fire the
	// press on contact. Returns 0 on a miss. Call as:
	//   int hit; Vector2 uv; double d; [hit, uv, d] = level.TouchBillboard(handPos, 8);
	native int, Vector2, double TouchBillboard(Vector3 point, double maxRange = 0);
	// Swept touch -- where a hand WAS to where it IS. Returns the first
	// billboard the path touches, its UV, and how far along the segment as a
	// 0..1 fraction. Returns 0 on a miss.
	//
	// TouchBillboard asks "is the hand in the panel now", and script only gets
	// to ask 35 times a second. A deliberate jab crosses a thin panel between
	// two tics without ever being inside it on either, so the gentle touch
	// works and the hard one does nothing. Sweeping the path closes that, and
	// gives "the hand ARRIVED this tic" for free instead of making callers
	// infer an edge from two samples.
	//
	// radius inflates the face into a slab and pads its edges, which is what a
	// fingertip is. Debounce, cooldown and which hand it was are deliberately
	// not here -- that is panel policy, not geometry.
	//   int hit; Vector2 uv; double f;
	//   [hit, uv, f] = level.SweepBillboard(lastHandPos, handPos, 2);
	native int, Vector2, double SweepBillboard(Vector3 from, Vector3 to, double radius = 0);

	// [BB] Volumetric beam -- a cone of light visible in the AIR, not just on
	// the surfaces it lands on. That is the difference between a flashlight
	// you can see the beam of and one you can only see the disc of.
	//
	// inner/outer are half-angles in degrees: full brightness inside inner,
	// faded to nothing by outer. falloff shapes the fade along the length --
	// 1 linear, higher concentrates the light near the lens. Publish it each
	// tic while the light is on; clear it when off, which costs nothing.
	// THIRTY-TWO SLOTS. Slot 0 is what a caller that never heard of slots gets,
	// so every call site written before slots existed keeps working. Before
	// slots they all shared one set of fields, so opening the weapon wheel put
	// your flashlight out.
	//
	// It was FOUR, and four was already full at rest for a dual-wielding VR
	// loadout: a flashlight, the wheel laser and a muzzle flash per hand, with
	// no headroom for anything added afterwards.
	//
	// Unused slots cost nothing to draw -- the post-process loops over the beams
	// actually published this frame, not over the ceiling -- so the limit is an
	// array bound rather than a budget.
	//
	// CONES, NOT LINES. These light the AIR in a volume: flashlights, muzzle
	// flashes. A laser SIGHT is a line and belongs in the separate 128-slot beam
	// system (SetBeam / SetBeamAnchor above), which is cheaper and can be
	// anchored to a hand.
	//
	// CLEARSCOPE because this is look-only render state, the same reason the
	// glow setters in mapdata.zs are: the playsim is frozen while a menu is
	// open, so a mod that can only publish the beam from play scope cannot show
	// a slider's change until the menu closes. From UiTick it can.
	//
	// A slot outside 0..31 is refused (logged), not moved to slot 0. Angles are
	// clamped to outer 0.1..89, inner at most outer - 0.1; length >= 1,
	// falloff >= 0.01 (logged when a clamp starts applying).
	native clearscope void SetVolumetricBeam(Vector3 pos, Vector3 dir, color col, double inner, double outer, double length, double density, double falloff, double dust = 0, double dustScale = 0.04, double dustDrift = 0, int slot = 0);
	// One slot, or every slot with -1. Defaults to 0 rather than to all of them:
	// a caller is turning off the beam it turned on, not everyone else's.
	// Clearing also releases the slot's anchor.
	native clearscope void ClearVolumetricBeam(int slot = 0);
	// WHERE THE CONE IS HELD, resolved every FRAME by the renderer rather than
	// every tic by script -- a hand moves at 90Hz+, script runs at 35, and a
	// torch posed from WorldTick judders behind the controller.
	//   mode 0  none: the pos/dir given to SetVolumetricBeam (default)
	//        1  main hand (AttackPos / AttackAngle / AttackPitch)
	//        2  off hand  (OffhandPos / OffhandAngle / OffhandPitch)
	//        3  head      (HmdPos / HmdYaw / HmdPitch; the view when no headset)
	// Anchored, BOTH origin and direction come from the pose: the cone points
	// along it, starting at offset = (forward, right, up) map units in that
	// pose's yaw/pitch frame (no roll). Poses are the console player's.
	// Call it AFTER SetVolumetricBeam each time you claim the slot: a slot that
	// was not live forgets its anchor when SetVolumetricBeam takes it.
	native clearscope void SetVolumetricBeamAnchor(int slot, int mode, Vector3 offset = (0, 0, 0));

	// [BB] Sweep -- up to eight thin bands of light travelling through the
	// world, each tested per pixel against world position on every surface,
	// so they wrap across floor, wall and ceiling as continuous unbroken
	// lines. Nothing per-sector can do this: a sector's glow is uniform
	// across that sector, while this is per-pixel in world space.
	//
	//   mode 0 off
	//        1 cylinder from origin -- rings expanding outward across a room
	//        2 plane along X        -- bars sweeping east/west down a corridor
	//                                  (both sides of the origin)
	//        3 plane along Y        -- the same, north/south
	//        4 sphere from origin   -- shells, so a band rises as it expands
	//        5 rise                 -- one sheet climbing along Z (signed)
	//        6 +X  7 +Y  8 -X  9 -Y -- one signed front crossing the level
	// [round2 SW-21] 5-9 were missing from this list.
	//
	// Draw modes (SetSweepBandDraw): 0 default/add, 1 add, 2 lift, 3 crush,
	// 4 recolour. Fill (SetSweepBandFill): 0 none, 1 grid, 2 dots, 3 solid slab,
	// 4 pickets. Uploaded as drawmode + 16*fill + 256*passed.
	//
	// Set the origin and how many bands are live, then each band's position
	// and colour. Drive the radii each tic: grow them for a ping, oscillate
	// for a sweep, stagger them for a train chasing itself down a corridor.
	//
	// [round2 X1] CLEARSCOPE, and they were play-scope for no reason. Every one
	// of these only stores render state: no playsim code reads the sweep --
	// band light, fog bow, glow wave and air lattice are all shader side -- so a
	// UiTick may push them (RS_Sweeps already does) and a menu slider moves the
	// picture while the playsim is paused, as with the glow and darkness
	// setters. A mod that makes things HAPPEN when a front passes does that from
	// its own play code, from its own record of where the front is.
	native clearscope void SetSweepOrigin(int mode, Vector3 origin, int count);
	native clearscope void SetSweepBand(int index, double radius, double thickness, double softness, color col, double intensity);
	native clearscope void SetSweepBandDraw(int index, int drawmode);
	native clearscope void SetSweepCount(int count);
	native clearscope void SetSweepBandAt(int index, Vector3 origin, int shape);
	native clearscope void SetSweepTrail(double trail);
	native clearscope void ClearSweep();

	// [BB] Glow wave: peaks and valleys along a glow, per pixel. Reach moves
	// the band's edge, brightness moves its light, colour moves the two-colour
	// boundary inside a band that never changes shape. Phases are per channel,
	// so offsetting them makes one wave climb a room. Wavelength 0 = off.
	//
	// CLEARSCOPE. These are render settings, not simulation: nothing
	// downstream of them can change what happens in the world, so a menu may
	// push them while the playsim is paused and a slider moves the picture as
	// it is dragged.
	//
	// [round2 X1] The origin too. It used to be play-scope because resolving
	// "follows you" or "the nearest live monster" means reading the world --
	// but that reading happens in the CALLER. The setter only stores a point the
	// shader reads, so its scope protected nothing. A caller that resolves the
	// origin from the world still does it from its world tic, and the point
	// keeps its last value while the game is stopped.
	native clearscope void SetGlowWave(double wavelength, double speed, double sharpness, int shape);
	native clearscope void SetGlowWaveOrigin(Vector3 origin);
	native clearscope void SetGlowWaveDepth(double reach, double bright, double colour, double detune, double seed);
	native clearscope void SetGlowWavePhase(double wallTop, double wallBottom, double floorPhase, double ceilPhase);
	native clearscope void ClearGlowWave();

	// [BB] Darkness as a shader term. The same four curves a mod would run per
	// sector per tic, evaluated instead against each fragment's own light --
	// plus distance and height, which a sector cannot express at all.
	// Mode 0 = off. Clearscope for the same reason as above.
	native clearscope void SetDarkness(int mode, double adjust, double minLight, double preGain, double postGain);
	native clearscope void SetDarknessSpace(double distDepth, double distRange, double heightDepth, double heightRef, double heightRange);
	// [round2 B4] Where heightRef comes from. mode 0 uses SetDarknessSpace's
	// value as written (as before); mode 1 follows the viewer's feet at draw
	// rate -- the camera's interpolated Z plus offset -- so the pool edge rides
	// lifts and stairs smoothly instead of stepping with a WorldTick write.
	// ClearDarkness sets it back to 0.
	native clearscope void SetDarknessHeightFollow(int mode, double offset);
	// [BB] How much of the darkness actors are spared, 0 to 1. The darkness pass
	// takes the whole scene down together, monsters included; 0 is that old
	// behaviour, 1 leaves actors at full brightness, and the useful settings are
	// in between.
	native clearscope void SetDarknessActors(double exempt);
	// [BB] Fog by room type. A sky ceiling is outdoors, anything else indoors.
	// Both default to 1, which is the fog exactly as configured.
	native clearscope void SetFogZones(double indoor, double outdoor);
	native clearscope void ClearDarkness();

	// [BB] Fog with a top -- a horizontal slab of mist with a world-space
	// ceiling, so you can stand knee deep in it and look down at its surface.
	// Density 0 = off. Scatter lets the flashlight cone light it.
	// The wake is one lagging point that thins the mist where you just walked.
	native clearscope void SetFogSlab(double topZ, double density, double softness, double scatter, color col);
	native void SetFogWake(Vector3 pos, double radius, double strength);
	// [RS fork] The wake split by scope: its SHAPE is look-only, so a menu's
	// UiTick can push it while the game is paused; its POSITION reads the
	// playsim and stays play scope. SetFogWake still sets all three at once.
	native clearscope void SetFogWakeShape(double radius, double strength, double stretch);
	native void SetFogWakePos(Vector3 pos);
	native clearscope void SetFogPickup(double amount);
	// A tornado -- the same fog shaped into a funnel you can stand inside.
	// Density 0 = off.
	native clearscope void SetTornado(double x, double y, double baseZ, double topZ, double radBase, double radTop, double density);
	native clearscope void SetTornadoMotion(double swirl, double spin, double twist, double lean, double leanPeriod);
	native clearscope void SetTornadoLook(color col, double scatter);

	// [BB] DISTURBANCES -- one primitive, five effects. A wake, a ripple, an
	// ignition, fog draining from a point and a monster shouldering mist
	// aside are the same function: a point, a radius, an age, a strength and
	// a sign. 32 slots, oldest recycled, strength decaying over its life.
	//
	//   mode 0 DISC    fixed radius, thins the mist. Wakes and displacers.
	//   mode 1 RIPPLE  a ring travelling out at r = age * speed
	//   mode 2 IGNITE  an expanding sphere that adds LIGHT, not density
	//   mode 3 GOUT    an expanding disc that ADDS mist -- a vent, a burst
	native clearscope void FogDisturb(double x, double y, double z, double radius, double strength, double speed, double life, int mode);
	native clearscope void ClearFogDisturb();
	native clearscope void SetFogNoise(double scale, double depth, double driftX, double driftY);
	native clearscope void SetFogTendrils(double spacing, double radius, double height, double density, double rise, double spread, double lean, double taper);
	native clearscope void SetFogWakeMotion(double velX, double velY, double stretch);
	native clearscope void SetFogBow(double strength, double width, double thin);
	// Which reference each fog edge follows, and how gently. 0 absolute,
	// positive follows the floor, negative the ceiling. The magnitude is the
	// gentleness -- 0.3 turns a staircase into a slope rather than steps.
	native clearscope void SetFogFollow(double top, double bottom);
	native clearscope void SetFogGradient(color col, double mix);
	// [round2 B3] A transient gradient over the standing one, for a caller that
	// comes and goes (a sweep tinting the mist) beside a fog mod that re-pushes
	// SetFogGradient every tic. Same arguments. While set it wins, and
	// ClearFogGradientOverride hands the view back to the standing gradient,
	// which it never touched. Mix 0 is a valid override (one colour while it
	// lasts). One override, not a stack; a map change ends it.
	native clearscope void SetFogGradientOverride(color col, double mix);
	native clearscope void ClearFogGradientOverride();
	// [RS fork] The colour an IGNITE disturbance (mode 2) burns. Unset, or
	// after ClearFogIgniteColor, it follows the gradient colour as before.
	native clearscope void SetFogIgniteColor(color col);
	native clearscope void ClearFogIgniteColor();

	// [BB] SHAPES -- signed distance fields drawn onto surfaces. 128 slots,
	// oldest EXPIRING one recycled. AddShape returns its slot so the other
	// four can address it later; -1 means it was refused.
	//
	// COST IS BOUNDED BY THE HIGHEST LIVE INDEX, not by the live count: the
	// shader loops to a high-water mark. So one permanent shape parked at slot
	// 120 costs 121 iterations per fragment for the rest of the map. Keep
	// anything with life 0 at low indices.
	//
	// And if every slot holds a life-0 permanent there is nothing to recycle,
	// so the allocator returns slot 0 and overwrites it rather than refusing.
	//
	//   kind    1 disc, 2 ring, 3 square, 4 square outline,
	//           5 cross, 6 hexagon, 7 triangle
	//   orient  0 floors, 1 walls, 2 any surface, 3 standing (freestanding
	//           in open air, not painted on anything -- see below)
	//   angle   in-plane rotation for orient 0-2; for orient 3, the
	//           direction the standing plane FACES instead (rotation
	//           around world-up -- it stands, it does not tilt)
	//   life    seconds; 0 never expires
	//
	// A seam SPLITS a shape down its middle and shows the under colour
	// through the gap -- subtraction, masked by the shape it came out of.
	//
	// ORIENT 3 IS A DIFFERENT KIND OF THING FROM 0-2. Those three are decals:
	// they only ever draw where a surface the game already rendered passes
	// through them. A standing shape has no surface to borrow -- it defines
	// its own vertical plane and is visible in open air, tested against the
	// eye's own view ray the same way a beam is, and correctly hidden behind
	// real geometry in front of it. See StandingShapesAt() in main.fp.
	native int AddShape(int kind, int orient, double x, double y, double z, double size, double angle, double thick, color col, double intensity, double life);
	native void SetShapeMotion(int slot, double seam, double seamRate, double grow);
	// One slot, many copies. mode 1 radial (count around a circle of radius
	// space, spinning), mode 2 grid (tiled every space units, out to count).
	// Folds the coordinate rather than drawing N shapes, so eight and eight
	// hundred cost the same -- but every copy is identical, which is why this
	// does not replace a slot per distinct event.
	native void SetShapeRepeat(int slot, int mode, double count, double space, double spin);
	native void MoveShape(int slot, double x, double y, double z);

	// [BB] STANDING SHAPES ONLY (orient 3) -- full 3D orientation and
	// linking. Base yaw is still AddShape's own angle; these add pitch and
	// roll, plus a rate for all three so a shape can spin or tumble without
	// polling it every tic (resolved the same way SetShapeMotion's seamRate
	// and grow already are: base + rate * age, once per frame, natively).
	native void SetShapeOrient(int slot, double pitch, double roll, double yawRate, double pitchRate, double rollRate);

	// One shape's world transform composed with its parent's -- move or
	// rotate the parent and everything linked to it comes with it, which is
	// how a compound 3D object gets built out of flat panels.
	//
	// parentSlot MUST be a smaller index than slot -- there is no cycle
	// check and no topological sort, only a single forward pass that
	// assumes every parent was already resolved before its children. -1
	// clears the link.
	//
	// The local offset composes along the PARENT's own resolved facing/
	// right/up, not world axes, so "64 units out, turned 90 degrees" means
	// the same thing whichever way the parent itself is currently turned.
	// Local yaw/pitch/roll ADD to the parent's resolved orientation -- exact
	// for a pure-yaw chain (a fan of panels around one vertical hinge),
	// an approximation once pitch and roll combine at the same joint.
	native void LinkShape(int slot, int parentSlot, double lx, double ly, double lz, double lyaw, double lpitch, double lroll);

	native void RemoveShape(int slot);
	native void ClearShapes();

	// [BB] The box the sweep's air lattice may exist inside, in world units.
	//
	// The lattice is an infinite plane, so without a room it stands wherever
	// that plane reaches and a window shows lasers in a room the sweep never
	// entered. Publish the extent of whatever you consider "the room" and it
	// fades out past it.
	//
	// soft is the fade distance in units. soft <= 0 removes the bound
	// entirely, which is the default and the behaviour of any map that never
	// calls this.
	//
	// Deliberately script-side: which sectors make up one room is a
	// judgement -- a Doom room is usually several, and whether a window or a
	// step ends it has no single right answer -- so the caller decides.
	native void SetSweepRoom(double minx, double miny, double minz, double maxx, double maxy, double maxz, double soft);
	native clearscope void SetShapeLook(double soft, double heightFade, double reach, color under);

	// [BB] TEXTURE INSIDE THE GLOW. The wave varies a glow's EDGE and has
	// nothing left to say once reach saturates and the edge is off screen.
	// These happen WITHIN the lit area, as multipliers on its contribution,
	// so none of them can move a band's shape. All off at 0.
	native clearscope void SetGlowTexture(double noise, double scale, double drift, double contrast);
	native clearscope void SetGlowFlow(double amount, double spacing, double speed, double sharp);
	native clearscope void SetGlowCells(double amount, double scale, double speed, double width);
	// rate multiplies the beat rate the level implies; 1 = the old rate, so
	// bright-and-slow is expressible. [RS fork]
	native clearscope void SetGlowReact(double react, double pulse, double level, double rate = 1.0);

	// [BB] WHAT SURVIVES A COLOUR DRAIN. Desaturation was all or nothing, so a
	// monochrome world made blood exactly as grey as the wall behind it.
	// Weighting the drain by each colour's own saturation keeps the vivid
	// things and drains everything else, with nothing tagged.
	//
	// hue: 0 any, 1 red-dominant only, 2 green, 3 blue.
	// threshold 0 restores the old behaviour exactly.
	native clearscope void SetDesatKeep(double threshold, double soft, int hue);

	// The drain itself, scene-global. SetDesatKeep decides what survives it.
	// 0 leaves desaturation entirely to the sector colormaps, as before.
	native clearscope void SetDesatGlobal(double amount);

	// [BB] THE HEATMAP -- where the fighting happened, accumulated over the
	// whole life of a map and drawn on the floor. A grid rather than a slot
	// array, because the question it answers is a spatial SUM: the thousandth
	// death costs what the first one cost.
	//
	// HeatmapAt reads it back, so a spawn director can weight against ground
	// that has already been fought over. That is what makes it a design tool
	// rather than only a picture.
	native void HeatmapAdd(double x, double y, double z, double radius, double amount);
	native void HeatmapClear();
	native clearscope void SetHeatmap(double scale, color low, color high, double ceiling, double decay, double tolerance);
	native clearscope double HeatmapAt(double x, double y);
	// The layer's bottom. Far below any map = fog on the floor, as before.
	// Raise it for ceiling fog, a floating band, or a drain.
	native clearscope void SetFogBottom(double botZ, double period, double roll);
	// The mist's surface, animated. Amplitude 0 = a flat top.
	native clearscope void SetFogSurface(double amp, double wavelength, double speed, double crossSwell);
	native clearscope void ClearFogSlab();
	// [RS fork] A TRANSIENT slab that wins over the standing one while set, and
	// leaves it untouched: for mist that comes and goes (a menu, a cutscene)
	// next to a fog mod that re-pushes SetFogSlab every tic. Absolute world Z,
	// its own bottom, no follow/stack/swell. Density <= 0 clears it.
	native clearscope void SetFogSlabOverride(double topZ, double density, double softness, double scatter, color col, double bottomZ = -32768);
	native clearscope void ClearFogSlabOverride();

	// [BB] The pattern drawn INSIDE a sweep band. Spacing 0 in an axis means
	// no lines in that axis, so grid / slats / a single tripwire are one mode.
	// The band's own colour is the field; this colour is the lines. Gap 0 =
	// only the lines are lit and the room shows between them.
	// Per band: 0 none, 1 grid, 2 dots, 3 solid slab, 4 pickets (bars along U
	// only, snapped to the room; on surfaces too since [round2 C1]).
	native clearscope void SetSweepFill(double spacingU, double spacingV, double width, double soft, color col, double gap);
	native clearscope void SetSweepFillMotion(double rotate, double drift, double major, double majorBoost, double jitter, double flicker, double grad, int gradAxis);
	native clearscope void SetSweepBandFill(int index, int fill);
	// [round2 B2] THE PASSED REGION. A band with its passed bit on grades
	// everything its front has already crossed, per pixel, so the look follows
	// the line exactly instead of flipping a sector when its centre crosses.
	// on: 0 off, 1 grade this band's passed side. The look is shared by all such
	// bands: tintMix multiplies the room's light toward tint, darken takes light
	// away, desat drains colour (all 0..1); soft is the map units the look fades
	// in over behind the front. It grades room light only -- glow, bands, beams
	// and fullbright draws are untouched -- and lasts while the band is live:
	// park a finished sweep past the far edge to keep it. Gameplay that must
	// outlive the sweep belongs in the mod. ClearSweep turns the bits off.
	native clearscope void SetSweepBandPassed(int index, int on);
	native clearscope void SetSweepPassedLook(color tint, double tintMix, double darken, double desat, double soft);
	// How strongly the band's lattice is drawn IN THE AIR rather than only on
	// the surfaces it lands on. 0 = painted only.
	native clearscope void SetSweepFillAir(double amount);

	// [BB] Real beams. A segment lit per pixel by distance from it, so it is
	// continuous at any length, wraps floor/wall/ceiling as one object, and
	// lights the surfaces it passes -- no sprite, no chain of puffs, no
	// separate dynamic light. Up to 128. thick is the hot core, soft is how
	// far the halo reaches past it.
	//
	// Cost is per ACTIVE beam, not per slot: both shader loops break at the
	// live count and each survivor gets a bounding-sphere reject first. An
	// empty slot costs nothing.
	//
	// The index space is CALLER-MANAGED with no allocator, so two mods writing
	// beams will silently overwrite each other. Agree a range.
	// [STAMP] Surface stamps -- an emissive shape painted onto whatever real
	// surface is at a world point. Spawn one and forget it: the engine ages it
	// and drops it when its life runs out. No handle comes back because there
	// is nothing to keep writing to, which is what makes it different from a
	// beam slot above.
	//
	//   shape    STAMP_* -- see func_surfacestamps.fp for the list
	//   pos      where it happened
	//   radius   how far the shape reaches, map units
	//   life     tics it lives for
	//   axis     world direction for the oriented shapes; zero for the rest
	//   tex      the optional second layer, STAMPTEX_*; 0 for none
	//   texStr   that layer's strength, 0..1
	//
	// clearscope so a menu can preview one while the game is paused, for the
	// same reason the glow setters on Sector are.
	native clearscope void SpawnSurfaceStamp(int shape, Vector3 pos, double radius, color col, int life, Vector3 axis, int tex = 0, double texStr = 0.0);
	native clearscope void ClearSurfaceStamps();

	// [GPUPARTICLES] Stateless GPU-drawn particles: sparks, embers, ejecta.
	// Fire and forget, and it NEVER refuses -- the oldest records are
	// overwritten when the ring is full, because a refusal would make the
	// hundredth spark in a firefight silently do nothing. Additive and emissive.
	// Vulkan only: on GL the records are written and never drawn. Jitter comes
	// from a hash of (seed, i), never from playsim RNG, so this is netplay-safe.
	//
	//   pos, dir       where, and the cone's axis (game space; dir need not be unit)
	//   count          how many; clamped to the ring size
	//   spread         cone HALF-angle in degrees around dir; 180 = every direction
	//   speed          map units per SECOND; speedJitter 0..1 -> speed * (1 +/- j)
	//   col, intensity emissive colour, brightness (additive; above 1 is allowed)
	//   life           seconds; lifeJitter 0..1 -> life * (1 +/- j)
	//   sizeStart/End  diameter in map units across its life (r_gpuparticles_maxsize caps it)
	//   gravity        map units per second^2, downward (Doom's own is about 1225)
	//   drag           1/s exponential slow-down; 0 = none
	//   orient         0 camera-facing, 1 stretched along velocity, 2 flat, facing its initial direction
	//   stretch        orient 1: seconds of travel drawn as the streak (try 0.02 - 0.05)
	//   seed           0 derives one from the ring cursor
	//
	// clearscope, like the stamps, so a menu can preview a burst while paused
	// (paused particles hold still -- they age by level time).
	native clearscope void SpawnGpuParticles(Vector3 pos, Vector3 dir, int count, double spread, double speed, double speedJitter, color col, double intensity, double life, double lifeJitter, double sizeStart, double sizeEnd, double gravity, double drag, int orient = 0, double stretch = 0, int seed = 0);
	native clearscope void ClearGpuParticles();

	native clearscope void SetBeam(int index, Vector3 start, Vector3 end, double thick, double soft, color col, double intensity);
	// WHERE THIS BEAM STARTS FROM: 0 the point given to SetBeam, 1 the main
	// hand, 2 the off hand. Anchored, the origin is resolved every FRAME rather
	// than every tic -- which is what a laser sight on a tracked controller
	// needs, because the hand moves at 90Hz+ and script runs at 35.
	native clearscope void SetBeamAnchor(int index, int mode);
	native clearscope void SetBeamCount(int count, double glow, double fogScatter);
	// airGlow 0 = the beam only lights what it touches. Above 0 it is visible
	// in the air as an object, depth-correct, and it feeds bloom by itself.
	native clearscope void SetBeamLook(double airGlow, double scrollSpeed, double scrollDepth, double taper, double flare);
	native clearscope void ClearBeams();

	// [BEAMLINES] A beam line of your own, out of the BeamCount fight.
	// See "Engine docs/BEAM_LINES_PLAN.md".
	//
	// ClaimBeam returns a free slot -- searched from 127 down, never below the
	// current BeamCount -- or -1. A claimed slot draws whatever BeamCount says,
	// and ClearBeams() leaves it alone. Write it with SetBeam / SetBeamAnchor as
	// usual. ReleaseBeam gives it back and blanks it. Claims do not survive a
	// map change or a savegame load: a caller holding one checks IsBeamClaimed
	// and claims again.
	//
	// play, not clearscope: which slot a claim gets depends on every claim made
	// before it, so a claim from UI code (which runs on one machine only) would
	// hand gameplay callers different slots on different machines.
	//
	// Pass the actor the line belongs to (a weapon, its owner) and the slot is
	// released by itself on the tic that actor is destroyed.
	native play int ClaimBeam(Actor owner = null);
	native play void ReleaseBeam(int index);
	native clearscope bool IsBeamClaimed(int index);
	// A look for ONE line, over the scene look from SetBeamCount / SetBeamLook.
	// Any slot, claimed or not. Scroll speed/depth and fog scatter stay
	// scene-wide. ClearBeamStyle puts the line back on the scene look.
	native clearscope void SetBeamStyle(int index, double airGlow, double halo, double taper, double flare);
	native clearscope void ClearBeamStyle(int index);

	// [DRAWNLINES] MANY glowing lines. The same look as a beam's glow in the air
	// -- the same maths, run only in a small box around each line -- so a
	// thousand cost what their pixels cost, not pixels x lines. What they do NOT
	// do is light surfaces; SetBeam lines still do. Vulkan only: on GL they are
	// stored and never drawn. Main view only, not mirrors or portals.
	//
	// Index space is caller-managed, 0 .. DrawnLineCapacity()-1, like SetBeam's.
	// A line persists until changed or cleared and is interpolated between tics.
	// thick is the hot core and soft the halo reach, exactly as SetBeam's -- and
	// the arguments come in SetBeam's order, so a SetBeam call renamed to
	// SetDrawnLine means the same thing.
	native clearscope void SetDrawnLine(int index, Vector3 start, Vector3 end, double thick, double soft, color col, double intensity);
	// This line's look. Defaults are the beam system's own; scrollDepth 0 is a
	// smooth line. airGlow <= 0 hides it -- a drawn line is only its glow.
	native clearscope void SetDrawnLineLook(int index, double airGlow = 1.0, double halo = 0.35, double taper = 0.35, double flare = 1.5, double scrollSpeed = 6.0, double scrollDepth = 0.0);
	// 0 the start given to SetDrawnLine, 1 the main hand, 2 the off hand --
	// resolved every frame, exactly as SetBeamAnchor. owner is whose hand: a
	// player's pawn. Without one it is the local player's hand, which in netplay
	// is wrong for every line that belongs to someone else.
	native clearscope void SetDrawnLineAnchor(int index, int mode, Actor owner = null);
	// Clearing forgets the slot completely, look and anchor included.
	native clearscope void ClearDrawnLine(int index);
	native clearscope void ClearDrawnLines();
	// A fixed engine number, the same on every machine, so it is safe for
	// gameplay code to size a ring of indices by it.
	native clearscope int DrawnLineCapacity();

	native String GetChecksum() const;

	native void ChangeSky(TextureID sky1, TextureID sky2 );
	native void ChangeSkyMist(TextureID skymist, bool usemist = true, float skymistyscale = 1.0);
	native void SetSkyFog(int fogdensity);
	native void SetThickFog(float distance, float multiplier);
	native void ForceLightning(int mode = 0, sound tempSound = "");

	native play Thinker CreateThinker(class<Thinker> type, int statnum = Thinker.STAT_DEFAULT);
	native clearscope Thinker CreateClientSideThinker(class<Thinker> type, int statnum = Thinker.STAT_DEFAULT);
	native clearscope SectorTagIterator CreateSectorTagIterator(int tag, line defline = null);
	native clearscope LineIdIterator CreateLineIdIterator(int tag);
	native clearscope ActorIterator CreateActorIterator(int tid, class<Actor> type = "Actor", bool clientSide = false);

	String TimeFormatted(bool totals = false)
	{
		int sec = Thinker.Tics2Seconds(totals? totaltime : time);
		return String.Format("%02d:%02d:%02d", sec / 3600, (sec % 3600) / 60, sec % 60);
	}

	native play bool CreateCeiling(sector sec, int type, line ln, double speed, double speed2, double height = 0, int crush = -1, int silent = 0, int change = 0, int crushmode = 0 /*Floor.crushDoom*/);
	native play bool CreateFloor(sector sec, int floortype, line ln, double speed, double height = 0, int crush = -1, int change = 0, bool crushmode = false, bool hereticlower = false);

	native void ExitLevel(int position, bool keepFacing);
	native void SecretExitLevel(int position);
	native void ChangeLevel(string levelname, int position = 0, int flags = 0, int skill = -1);

	native String GetClusterName();
	native String GetEpisodeName();

	native clearscope void SpawnParticle(FSpawnParticleParams p);
	native play VisualThinker SpawnVisualThinker(Class<VisualThinker> type);
	native clearscope VisualThinker SpawnClientSideVisualThinker(Class<VisualThinker> type);

	clearscope native static bool WorldPaused(bool checkLag = false);
}

// a few values of this need to be readable by the play code.
// Most are handled at load time and are omitted here.
struct DehInfo native
{
	native readonly int MaxSoulsphere;
	native readonly uint8 ExplosionStyle;
	native readonly double ExplosionAlpha;
	native readonly int NoAutofreeze;
	native readonly int BFGCells;
	native readonly int BlueAC;
	native readonly int MaxHealth;
}

struct State native
{
	native readonly State NextState;
	native readonly int sprite;
	native readonly int16 Tics;
	native readonly uint16 TicRange;
	native readonly uint8 Frame;
	native readonly uint8 UseFlags;
	native readonly int Misc1;
	native readonly int Misc2;
	native readonly uint16 bSlow;
	native readonly uint16 bFast;
	native readonly bool bFullbright;
	native readonly bool bNoDelay;
	native readonly bool bSameFrame;
	native readonly bool bCanRaise;
	native readonly bool bDehacked;

	native int DistanceTo(state other) const;
	native bool ValidateSpriteFrame() const;
	native TextureID, bool, Vector2 GetSpriteTexture(int rotation, int skin = 0, Vector2 scale = (0,0), int spritenum = -1, int framenum = -1) const;
	native bool InStateSequence(State base) const;
}

struct TerrainDef native
{
	native Name TerrainName;
	native int Splash;
	native int DamageAmount;
	native Name DamageMOD;
	native int DamageTimeMask;
	native double FootClip;
	native float StepVolume;
	native int WalkStepTics;
	native int RunStepTics;
	native Sound LeftStepSound;
	native Sound RightStepSound;
	native bool IsLiquid;
	native bool AllowProtection;
	native bool DamageOnLand;
	native double Friction;
	native double MoveFactor;
	native Sound StepSound;
	native double StepDistance;
	native double StepDistanceMinVel;
};

enum EPickStart
{
	PPS_FORCERANDOM			= 1,
	PPS_NOBLOCKINGCHECK		= 2,
}


enum EMissileHitResult
{
	MHIT_DEFAULT = -1,
	MHIT_DESTROY = 0,
	MHIT_PASS = 1,
}

class SectorEffect : Thinker native
{
	native protected Sector m_Sector;

	native Sector GetSector();
}

class Mover : SectorEffect native
{}

class Elevator : Mover native
{
	enum EElevator
	{
		elevateUp,
		elevateDown,
		elevateCurrent,
		// [RH] For FloorAndCeiling_Raise/Lower
		elevateRaise,
		elevateLower
	};

	native readonly EElevator	m_Type;
	native readonly int			m_Direction;
	native readonly double		m_FloorDestDist;
	native readonly double		m_CeilingDestDist;
	native readonly double		m_Speed;
}

class MovingFloor : Mover native
{}

class Plat : MovingFloor native
{
	enum EPlatState
	{
		up,
		down,
		waiting,
		in_stasis
	};

	enum EPlatType
	{
		platPerpetualRaise,
		platDownWaitUpStay,
		platDownWaitUpStayStone,
		platUpWaitDownStay,
		platUpNearestWaitDownStay,
		platDownByValue,
		platUpByValue,
		platUpByValueStay,
		platRaiseAndStay,
		platToggle,
		platDownToNearestFloor,
		platDownToLowestCeiling,
		platRaiseAndStayLockout,
	};

	bool IsLift() const { return m_Type == platDownWaitUpStay || m_Type == platDownWaitUpStayStone; }

	native readonly double m_Speed;
	native readonly double m_Low;
	native readonly double m_High;
	native readonly int m_Wait;
	native readonly int m_Count;
	native readonly EPlatState m_Status;
	native readonly EPlatState m_OldStatus;
	native readonly int m_Crush;
	native readonly int m_Tag;
	native readonly EPlatType m_Type;
}

class MovingCeiling : Mover native
{}

class Door : MovingCeiling native
{
	enum EVlDoor
	{
		doorClose,
		doorOpen,
		doorRaise,
		doorWaitRaise,
		doorCloseWaitOpen,
		doorWaitClose,
	};

	native readonly EVlDoor	m_Type;
	native readonly double	m_TopDist;
	native readonly double	m_BotDist, m_OldFloorDist;
	native readonly Vertex	m_BotSpot;
	native readonly double	m_Speed;

	// 1 = up, 0 = waiting at top, -1 = down
	enum EDirection
	{
		dirDown = -1,
		dirWait,
		dirUp,
	}
	native readonly int		m_Direction;

	// tics to wait at the top
	native readonly int		m_TopWait;
	// (keep in case a door going down is reset)
	// when it reaches 0, start going down
	native readonly int		m_TopCountdown;

	native readonly int		m_LightTag;
}

class Floor : MovingFloor native
{
	// only here so that some constants and functions can be added. Not directly usable yet.
	enum EFloor
	{
		floorLowerToLowest,
		floorLowerToNearest,
		floorLowerToHighest,
		floorLowerByValue,
		floorRaiseByValue,
		floorRaiseToHighest,
		floorRaiseToNearest,
		floorRaiseAndCrush,
		floorRaiseAndCrushDoom,
		floorCrushStop,
		floorLowerInstant,
		floorRaiseInstant,
		floorMoveToValue,
		floorRaiseToLowestCeiling,
		floorRaiseByTexture,

		floorLowerAndChange,
		floorRaiseAndChange,

		floorRaiseToLowest,
		floorRaiseToCeiling,
		floorLowerToLowestCeiling,
		floorLowerByTexture,
		floorLowerToCeiling,

		donutRaise,

		buildStair,
		waitStair,
		resetStair,

		// Not to be used as parameters to DoFloor()
		genFloorChg0,
		genFloorChgT,
		genFloorChg
	};

	enum EStair
	{
		buildUp,
		buildDown
	};

	enum EStairType
	{
		stairUseSpecials = 1,
		stairSync = 2,
		stairCrush = 4,
	};

	native readonly EFloor			m_Type;
	native readonly int				m_Crush;
	native readonly bool			m_Hexencrush;
	native readonly bool			m_Instant;
	native readonly int				m_Direction;
	native readonly SecSpecial		m_NewSpecial;
	native readonly TextureID		m_Texture;
	native readonly double			m_FloorDestDist;
	native readonly double			m_Speed;

	// [RH] New parameters used to reset and delay stairs
	native readonly double			m_OrgDist;
	native readonly int				m_ResetCount;
	native readonly int				m_Delay;
	native readonly int				m_PauseTime;
	native readonly int				m_StepTime;
	native readonly int				m_PerStepTime;

	deprecated("3.8", "Use Level.CreateFloor() instead") static bool CreateFloor(sector sec, int floortype, line ln, double speed, double height = 0, int crush = -1, int change = 0, bool crushmode = false, bool hereticlower = false)
	{
		return level.CreateFloor(sec, floortype, ln, speed, height, crush, change, crushmode, hereticlower);
	}
}

class Ceiling : MovingCeiling native
{
	enum ECeiling
	{
		ceilLowerByValue,
		ceilRaiseByValue,
		ceilMoveToValue,
		ceilLowerToHighestFloor,
		ceilLowerInstant,
		ceilRaiseInstant,
		ceilCrushAndRaise,
		ceilLowerAndCrush,
		ceil_placeholder,
		ceilCrushRaiseAndStay,
		ceilRaiseToNearest,
		ceilLowerToLowest,
		ceilLowerToFloor,

		// The following are only used by Generic_Ceiling
		ceilRaiseToHighest,
		ceilLowerToHighest,
		ceilRaiseToLowest,
		ceilLowerToNearest,
		ceilRaiseToHighestFloor,
		ceilRaiseToFloor,
		ceilRaiseByTexture,
		ceilLowerByTexture,

		genCeilingChg0,
		genCeilingChgT,
		genCeilingChg
	}

	enum ECrushMode
	{
		crushDoom = 0,
		crushHexen = 1,
		crushSlowdown = 2
	}

	// 1 = up, 0 = waiting, -1 = down
	enum EDirection
	{
		dirDown = -1,
		dirWait,
		dirUp,
	}

	native readonly ECeiling	m_Type;
	native readonly double	 	m_BottomHeight;
	native readonly double	 	m_TopHeight;
	native readonly double	 	m_Speed;
	native readonly double		m_Speed1;		// [RH] dnspeed of crushers
	native readonly double		m_Speed2;		// [RH] upspeed of crushers
	native readonly ECrushMode	m_CrushMode;
	native readonly int			m_Silent;

	bool IsCrusher() const { return m_Type == ceilCrushAndRaise || m_Type == ceilLowerAndCrush || m_Type == ceilCrushRaiseAndStay; }
	native int getCrush() const;
	native int getDirection() const;
	native int getOldDirection() const;

	deprecated("3.8", "Use Level.CreateCeiling() instead") static bool CreateCeiling(sector sec, int type, line ln, double speed, double speed2, double height = 0, int crush = -1, int silent = 0, int change = 0, int crushmode = crushDoom)
	{
		return level.CreateCeiling(sec, type, ln, speed, speed2, height, crush, silent, change, crushmode);
	}

}

struct LookExParams
{
	double Fov;
	double minDist;
	double maxDist;
	double maxHeardist;
	int flags;
	State seestate;
};

class Lighting : SectorEffect native
{
}

struct Shader native
{
	// This interface was deprecated for the pointless player dependency
	private static bool IsConsolePlayer(PlayerInfo player)
	{
		return player && player.mo && player == players[consoleplayer];
	}
	deprecated("4.8", "Use PPShader.SetEnabled() instead") clearscope static void SetEnabled(PlayerInfo player, string shaderName, bool enable)
	{
		if (IsConsolePlayer(player)) PPShader.SetEnabled(shaderName, enable);
	}
	deprecated("4.8", "Use PPShader.SetUniform1f() instead") clearscope static void SetUniform1f(PlayerInfo player, string shaderName, string uniformName, float value)
	{
		if (IsConsolePlayer(player)) PPShader.SetUniform1f(shaderName, uniformName, value);
	}
	deprecated("4.8", "Use PPShader.SetUniform2f() instead") clearscope static void SetUniform2f(PlayerInfo player, string shaderName, string uniformName, vector2 value)
	{
		if (IsConsolePlayer(player)) PPShader.SetUniform2f(shaderName, uniformName, value);
	}
	deprecated("4.8", "Use PPShader.SetUniform3f() instead") clearscope static void SetUniform3f(PlayerInfo player, string shaderName, string uniformName, vector3 value)
	{
		if (IsConsolePlayer(player)) PPShader.SetUniform3f(shaderName, uniformName, value);
	}
	deprecated("4.8", "Use PPShader.SetUniform1i() instead") clearscope static void SetUniform1i(PlayerInfo player, string shaderName, string uniformName, int value)
	{
		if (IsConsolePlayer(player)) PPShader.SetUniform1i(shaderName, uniformName, value);
	}
}

struct FRailParams
{
	native int damage;
	native double offset_xy;
	native double offset_z;
	native int color1, color2;
	native double maxdiff;
	native int flags;
	native Class<Actor> puff;
	native double angleoffset;
	native double pitchoffset;
	native double distance;
	native int duration;
	native double sparsity;
	native double drift;
	native Class<Actor> spawnclass;
	native int SpiralOffset;
	native int limit;
};	// [RH] Shoot a railgun

// This is just here to prevent mods that used setting this directly from breaking.
struct DecalBase native {}
