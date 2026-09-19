/*
** p_setup.cpp
**
** Setup a game, startup stuff.
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

#include <math.h>
#include <QzDoom/VrCommon.h>
#ifdef _MSC_VER
#include <malloc.h>		// for alloca()
#endif


#include "d_player.h"
#include "m_argv.h"
#include "g_game.h"
#include "filesystem.h"
#include "p_local.h"
#include "p_effect.h"
#include "p_terrain.h"
#include "nodebuild.h"
#include "p_lnspec.h"
#include "c_console.h"
#include "p_acs.h"
#include "announcer.h"
#include "wi_stuff.h"
#include "engineerrors.h"
#include "gi.h"
#include "p_conversation.h"
#include "a_keys.h"
#include "s_sndseq.h"
#include "sbar.h"
#include "p_setup.h"
#include "r_data/r_interpolate.h"
#include "r_sky.h"
#include "cmdlib.h"
#include "md5.h"
#include "po_man.h"
#include "swrenderer/r_renderer.h"
#include "p_blockmap.h"
#include "r_utility.h"
#include "p_spec.h"
#include "g_levellocals.h"
#include "c_dispatch.h"
#include "a_dynlight.h"
#include "events.h"
#include "p_destructible.h"
#include "types.h"
#include "i_time.h"
#include "vm.h"
#include "a_specialspot.h"
#include "maploader/maploader.h"
#include "p_acs.h"
#include "am_map.h"
#include "i_system.h"
#include "v_video.h"
#include "fragglescript/t_script.h"
#include "s_music.h"
#include "animations.h"
#include "texturemanager.h"
#include "p_lnspec.h"
#include "d_main.h"

extern AActor *SpawnMapThing (int index, FMapThing *mthing, int position);

extern unsigned int R_OldBlend;


//===========================================================================
//
// P_PrecacheLevel
//
// Preloads all relevant graphics for the Level->
//
//===========================================================================
void hw_PrecacheTexture(uint8_t *texhitlist, TMap<PClassActor*, bool> &actorhitlist);

static void AddToList(uint8_t *hitlist, FTextureID texid, int bitmask)
{
	if (hitlist[texid.GetIndex()] & bitmask) return;	// already done, no need to process everything again.
	hitlist[texid.GetIndex()] |= (uint8_t)bitmask;

	const auto addAnimations = [hitlist, bitmask](const FTextureID texid)
	{
		for (auto& anim : TexAnim.GetAnimations())
		{
			if (texid == anim.BasePic || (!anim.bDiscrete && anim.BasePic < texid && texid < anim.BasePic + anim.NumFrames))
			{
				for (int i = anim.BasePic.GetIndex(); i < anim.BasePic.GetIndex() + anim.NumFrames; i++)
				{
					hitlist[i] |= (uint8_t)bitmask;
				}
			}
		}
	};

	addAnimations(texid);

	auto switchdef = TexAnim.FindSwitch(texid);
	if (switchdef)
	{
		const FSwitchDef *const pair = switchdef->PairDef;
		const uint16_t numFrames = switchdef->NumFrames;
		const uint16_t pairNumFrames = pair->NumFrames;

		for (int i = 0; i < numFrames; i++)
		{
			hitlist[switchdef->frames[i].Texture.GetIndex()] |= (uint8_t)bitmask;
		}
		for (int i = 0; i < pairNumFrames; i++)
		{
			hitlist[pair->frames[i].Texture.GetIndex()] |= (uint8_t)bitmask;
		}

		if (numFrames == 1 && pairNumFrames == 1)
		{
			// Switch can still be animated via BOOM binary definition from ANIMATED lump
			addAnimations(switchdef->frames[0].Texture);
			addAnimations(pair->frames[0].Texture);
		}
	}

	auto adoor = TexAnim.FindAnimatedDoor(texid);
	if (adoor)
	{
		for (int i = 0; i < adoor->NumTextureFrames; i++)
		{
			hitlist[adoor->TextureFrames[i].GetIndex()] |= (uint8_t)bitmask;
		}
	}
}

// [SELACO PRECACHE] Defined in hw_cvars.cpp beside gl_precache. This machine's choice of what to
// load; nothing in the playsim reads them.
EXTERN_CVAR(Bool, gl_precache_actors)
EXTERN_CVAR(Bool, debug_precache_actor)

static void PrecacheLevel(FLevelLocals *Level)
{
	if (demoplayback)
		return;

	int i;
	TMap<PClassActor *, bool> actorhitlist;
	int cnt = TexMan.NumTextures();
	TArray<uint8_t> hitlist(cnt, true);

	memset(hitlist.Data(), 0, cnt);

	AActor *actor;
	auto iterator = Level->GetThinkerIterator<AActor>();

	while ((actor = iterator.Next()))
	{
		actorhitlist[actor->GetClass()] = true;
	}

	// [SELACO PRECACHE] gl_precache_actors (GZSelaco 863a34c9b2) switches the GAMEINFO and MAPINFO
	// class lists. It defaults on, which is exactly what ran here before it existed.
	if (gl_precache_actors)
	{
		for (auto n : gameinfo.PrecachedClasses)
		{
			PClassActor *cls = PClass::FindActor(n);
			if (cls != nullptr) actorhitlist[cls] = true;
		}
		for (unsigned i = 0; i < Level->info->PrecacheClasses.Size(); i++)
		{
			// Level->info can only store names, no class pointers.
			PClassActor *cls = PClass::FindActor(Level->info->PrecacheClasses[i]);
			if (cls != nullptr) actorhitlist[cls] = true;
		}
	}

	// [SELACO PRECACHE] PRECACHEALWAYS (GZSelaco ff23e058c0): a class that loads on every map whether
	// or not one is placed in it -- the projectiles, flashes and casings a gun spawns mid-fight, whose
	// first appearance would otherwise load its textures and model on that frame. The flag is in the
	// class defaults, so a subclass inherits it. hw_PrecacheTexture also walks these classes'
	// inherited states. Not on a title map, which Selaco keeps quick; a game started straight into a
	// map is still GS_STARTUP here and does precache (GZSelaco 7d9a031ffc).
	// debug_precache_actor (not saved) adds every actor class, to measure the worst case.
	// Netplay: this picks textures and models to load on this machine. It spawns, changes and
	// orders nothing in the playsim, and reads only class defaults.
	if (gamestate != GS_TITLELEVEL)
	{
		unsigned always = 0;
		for (PClassActor *cls : PClassActor::AllActorClasses)
		{
			AActor *def = GetDefaultByType(cls);
			if (def != nullptr && (def->flags9 & MF9_PRECACHEALWAYS))
			{
				actorhitlist[cls] = true;
				always++;
			}
		}
		if (debug_precache_actor)
		{
			for (PClassActor *cls : PClassActor::AllActorClasses) actorhitlist[cls] = true;
		}
		if (always > 0 || debug_precache_actor)
		{
			DPrintf(DMSG_NOTIFY, "Precache: %u PRECACHEALWAYS actor classes%s\n", always,
				debug_precache_actor ? ", and every other class (debug_precache_actor)" : "");
		}
	}

	for (i = Level->sectors.Size() - 1; i >= 0; i--)
	{
		AddToList(hitlist.Data(), Level->sectors[i].GetTexture(sector_t::floor), FTextureManager::HIT_Flat);
		AddToList(hitlist.Data(), Level->sectors[i].GetTexture(sector_t::ceiling), FTextureManager::HIT_Flat);
		AddToList(hitlist.Data(), Level->sectors[i].planes[0].skytexture[0], FTextureManager::HIT_Wall);
		AddToList(hitlist.Data(), Level->sectors[i].planes[0].skytexture[1], FTextureManager::HIT_Wall);
		AddToList(hitlist.Data(), Level->sectors[i].planes[1].skytexture[0], FTextureManager::HIT_Wall);
		AddToList(hitlist.Data(), Level->sectors[i].planes[1].skytexture[1], FTextureManager::HIT_Wall);
	}

	for (i = Level->sides.Size() - 1; i >= 0; i--)
	{
		auto &sd = Level->sides[i];
		int hitflag = FTextureManager::HIT_Wall;
		// Only precache skyboxes when this is actually used as a sky transfer.
		if (sd.linedef->sidedef[0] == &sd && sd.linedef->special == Static_Init && sd.linedef->args[1] == Init_TransferSky) hitflag |= FTextureManager::HIT_Sky;
		AddToList(hitlist.Data(), sd.GetTexture(side_t::top), hitflag);
		AddToList(hitlist.Data(), sd.GetTexture(side_t::mid), FTextureManager::HIT_Wall);
		AddToList(hitlist.Data(), sd.GetTexture(side_t::bottom), hitflag);
	}


	if (Level->skytexture1.isValid())
	{
		AddToList(hitlist.Data(), Level->skytexture1, FTextureManager::HIT_Sky);
	}
	if (Level->skytexture2.isValid())
	{
		AddToList(hitlist.Data(), Level->skytexture2, FTextureManager::HIT_Sky);
	}
	if (Level->skymisttexture.isValid())
	{
		AddToList(hitlist.Data(), Level->skymisttexture, FTextureManager::HIT_Sky);
	}

	static const BITFIELD checkForTextureFlags = FTextureManager::TEXMAN_Overridable | FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ReturnFirst | FTextureManager::TEXMAN_DontCreate;

	for (auto n : gameinfo.PrecachedTextures)
	{
		FTextureID tex = TexMan.CheckForTexture(n.GetChars(), ETextureType::Wall, checkForTextureFlags);
		if (tex.Exists()) AddToList(hitlist.Data(), tex, FTextureManager::HIT_Wall);
	}
	for (unsigned i = 0; i < Level->info->PrecacheTextures.Size(); i++)
	{
		FTextureID tex = TexMan.CheckForTexture(Level->info->PrecacheTextures[i].GetChars(), ETextureType::Wall, checkForTextureFlags);
		if (tex.Exists()) AddToList(hitlist.Data(), tex, FTextureManager::HIT_Wall);
	}

	// This is just a temporary solution, until the hardware renderer's texture manager is in a better state.
	if (!V_IsHardwareRenderer())
		SWRenderer->Precache(hitlist.Data(), actorhitlist);
	else
		hw_PrecacheTexture(hitlist.Data(), actorhitlist);

}

//============================================================================
//
// clears all portal data for a new level start
//
//============================================================================

void FLevelLocals::ClearPortals()
{
	Displacements.Create(1);
	linePortals.Clear();
	linkedPortals.Clear();
	sectorPortals.Resize(2);
	PortalBlockmap.Clear();

	// The first entry must always be the default skybox. This is what every sector gets by default.
	sectorPortals[0].Clear();
	sectorPortals[0].mType = PORTS_SKYVIEWPOINT;
	sectorPortals[0].mFlags = PORTSF_SKYFLATONLY;
	// The second entry will be the default sky. This is for forcing a regular sky through the skybox picker
	sectorPortals[1].Clear();
	sectorPortals[1].mType = PORTS_SKYVIEWPOINT;
	sectorPortals[1].mFlags = PORTSF_SKYFLATONLY;

	// also clear the render data
	for (auto &sub : subsectors)
	{
		for (int j = 0; j < 2; j++)
		{
			if (sub.portalcoverage[j].subsectors != nullptr)
			{
				delete[] sub.portalcoverage[j].subsectors;
				sub.portalcoverage[j].subsectors = nullptr;
			}
		}
	}
	for (unsigned i = 0; i < portalGroups.Size(); i++)
	{
		delete portalGroups[i];
	}
	portalGroups.Clear();
	linePortalSpans.Clear();
}

//==========================================================================
//
//
//
//==========================================================================

void FLevelLocals::ClearLevelData(bool fullgc)
{
	{
		// [BB] Travelling actors keep pointers into the level we are about to
		// tear down. BlockingMobj is an actor pointer and is not covered by
		// the native-pointer sweep below, so it still has to be cleared here.
		auto it = GetThinkerIterator<AActor>(NAME_None, STAT_TRAVELLING);
		for (AActor *actor = it.Next(); actor != nullptr; actor = it.Next())
		{
			actor->BlockingMobj = nullptr;
			actor->BlockingLine = actor->MovementBlockingLine = nullptr;
			actor->BlockingFloor = actor->BlockingCeiling = actor->Blocking3DFloor = nullptr;
		}

		// Make sure map data gets cleared appropriately so any leftover Objects aren't pointing
		// towards anything invalid.
		FName fieldTypes[] = { NAME_SectorPortal, NAME_LinePortal, NAME_Vertex, NAME_Side, NAME_Line, NAME_SecPlane, NAME_F3DFloor, NAME_Sector };
		for (DObject* probe = GC::Root; probe != nullptr; probe = probe->ObjNext)
			probe->ClearNativePointerFields({ fieldTypes, std::size(fieldTypes) });
	}

	TravellingThinkers.Clear();
	interpolator.ClearInterpolations();	// [RH] Nothing to interpolate on a fresh level.

	// [GPUPARTICLES] No particle survives a map change or a savegame load.
	// maptime restarts, so a stale record would come back to life with a
	// valid-looking age; the fresh serial makes the renderer re-upload the
	// whole (now empty) ring. See FLevelLocals::ResetGpuParticles.
	ResetGpuParticles();

	// [BB] Beams do not survive a map change. Nothing else resets them, so
	// without this the first tic of the new map still holds the old map's
	// endpoints and would interpolate a beam from wherever it was standing in
	// the level you just left. Dropping the count is enough on its own -- the
	// renderer never looks past it, and the snapshot rebuilds from there.
	BeamCount = 0;
	PrevBeamCount = 0;
	for (int b = 0; b < MAX_BEAMS; b++)
	{
		PrevBeamIntensity[b] = 0.0;

		// [BEAMLINES] Claims and styles go with the map. A claimed slot is also
		// blanked and unanchored, so nothing on the new map inherits it.
		if (BeamClaimed[b])
		{
			BeamIntensity[b] = 0.0;
			BeamAnchor[b] = 0;
		}
		BeamClaimed[b] = false;
		BeamClaimOwned[b] = false;
		BeamClaimOwner[b] = nullptr;
		BeamHasStyle[b] = false;
		PrevBeamLive[b] = false;
	}
	BeamClaimFullLogged = false;
	BeamCountOverClaimLogged = false;

	// [DRAWNLINES] No drawn line survives a map change or a savegame load either.
	ClearDrawnLines();

	// [HEATREFRACTION] Nor does a heat source: its position is the old map's, and its
	// fade counts from the old map's clock.
	ClearHeatSources();

	// [SHOCKWAVE] Nor does a blast ripple, for the same two reasons.
	ClearShockwaves();

	// [EFFECTQUEUES] + [SMOKEVOLUME] Nor does a queued effect event (its position is the
	// old map's), nor the smoke look and wind a mod set (it re-applies them on
	// WorldLoaded). The new level-data serial tells renderer-side effects -- the smoke
	// volume, the sector plane poll -- that the level they built their state for is gone.
	ClearEffectQueues();
	ResetSmokeState();

	// RS FORK -- WORLD CLOCK: nor does a time scale survive a map change or a savegame
	// load. maptime restarts at 0 and so does the real clock beside it; a scale is a
	// mod's live decision, and a new map inheriting a half-frozen world from the last
	// one is a bug nobody would find. The load path writes the saved values back after.
	ResetWorldClock();
	LevelDataSerial = NewLevelDataSerial();

	// [RS fork] NOR DOES THE REST OF THE LEVEL'S VISUAL STATE.
	//
	// FLevelLocals is one static object, so everything a mod pushed -- a sweep,
	// a torch cone, darkness, a fog slab, a tornado -- was still set when the
	// next map started, drawn at the OLD map's coordinates until some mod pushed
	// again, and a mod that only sets these on events never does. Disturbance
	// and shape births are in map seconds and maptime restarts at 0, so an old
	// ripple sat hidden until the new map's clock reached its birth and then
	// fired at its old position.
	//
	// Only the switches that make something DRAW are reset, to their declared
	// defaults (all default-off), mirroring ClearSweep / ClearFogSlab /
	// ClearDarkness. Look settings (colours, spacings, curves) are left alone:
	// they draw nothing by themselves, and a mod may set them once.
	{
		FString live;
		if (SweepMode != 0 || SweepCount != 0) live += " sweep";
		for (int i = 0; i < MAX_VOL_BEAMS; i++) if (VolBeamActive[i]) { live += " volbeam"; break; }
		if (GlowWaveLength != 0) live += " glowwave";
		if (DarkMode != 0) live += " darkness";
		if (FogSlabActive) live += " fogslab";
		if (FogSlabOverrideActive) live += " fogslaboverride";
		if (BloomOverrideActive) live += " bloomoverride";   // [BLOOMOVERRIDE]
		if (FogColor2OverrideActive) live += " foggradientoverride";   // [round2 B3]
		if (TornadoDensity != 0) live += " tornado";
		if (FogBowStrength != 0) live += " fogbow";
		if (SweepRoomSoft != 0) live += " sweeproom";
		for (int i = 0; i < MAX_FOG_DISTURB; i++) if (FogDisturbLife[i] > 0) { live += " disturbances"; break; }
		for (int i = 0; i < MAX_SHAPES; i++) if (ShapeSize[i] > 0 || ShapeKind[i] != 0) { live += " shapes"; break; }

		SweepMode = 0;
		SweepCount = 0;
		SweepTrail = 0;
		for (int i = 0; i < MAX_SWEEP_BANDS; i++)
		{
			SweepRadius[i] = 0;
			SweepThickness[i] = 0;
			SweepSoftness[i] = 0;
			SweepColor[i] = 0;
			SweepIntensity[i] = 0;
			SweepBandOrigin[i] = DVector3(0, 0, 0);
			SweepBandMode[i] = 0;
			SweepBandDraw[i] = 0;
			SweepBandFill[i] = 0;
			SweepBandPassed[i] = 0;   // [round2 B2]
		}
		SweepFillAir = 0;
		SweepRoomSoft = 0;
		SweepRoomMin = SweepRoomMax = DVector3(0, 0, 0);

		// Anchors too: a slot anchored to a hand on the old map must not come
		// back anchored when a new caller claims it (the beam lane's
		// VolBeamAnchor / VolBeamAnchorOffset, same reset ClearVolumetricBeam does).
		for (int i = 0; i < MAX_VOL_BEAMS; i++)
		{
			VolBeamActive[i] = false;
			VolBeamAnchor[i] = 0;
			VolBeamAnchorOffset[i] = DVector3(0, 0, 0);
		}

		GlowWaveLength = 0;

		DarkMode = 0;
		DarkDistDepth = 0;
		DarkHeightDepth = 0;
		DarkHeightFollow = 0;   // [round2 B4]

		FogSlabActive = false;
		FogSlabDensity = 0;
		FogSlabWakeStrength = 0;
		FogSlabOverrideActive = false;   // a transient mist never outlives its map
		FogSlabOverrideDensity = 0;
		FogColor2OverrideActive = false; // [round2 B3] nor does a transient gradient
		BloomOverrideActive = false;     // [BLOOMOVERRIDE] nor does a transient bloom look
		TornadoDensity = 0;
		FogBowStrength = 0;

		for (int i = 0; i < MAX_FOG_DISTURB; i++) FogDisturbLife[i] = 0;
		for (int i = 0; i < MAX_SHAPES; i++) { ShapeSize[i] = 0; ShapeKind[i] = 0; }

		// Stamps own their own clear, and their array size is not ours to know.
		ClearSurfaceStamps();

		// One line per map change, and only when something was actually live.
		if (live.IsNotEmpty())
			Printf("Level visual state reset for the new map:%s\n", live.GetChars());
	}

	Thinkers.DestroyAllThinkers(fullgc);
	ClientSideThinkers.DestroyAllThinkers(fullgc);
	ClearAllSubsectorLinks(); // can't be done as part of the polyobj deletion process.

	// [round2 B1] The anchored-light registry (FDynamicLight::PoseAnchor). Every
	// light freed above left it through ReleaseLight, so anything still listed is
	// a live light: it is forgotten rather than freed, with its flag dropped so a
	// later SetPoseAnchor registers it again. Nothing can dangle into the next map.
	for (auto light : PoseAnchoredLights) light->PoseRegistered = false;
	PoseAnchoredLights.Clear();

	total_monsters = total_items = total_secrets =
	killed_monsters = found_items = found_secrets = 0;
	LocalTimer = LocalWorldTimer = 0;
	sky1pos = sky2pos = 0.0;
	hw_sky1pos = hw_sky2pos = hw_skymistpos = 0.0;
	TexAnim.ResetTimers();

	ClearVelocities();

	for (int i = 0; i < 4; i++)
	{
		UDMFKeys[i].Clear();
	}

	SN_StopAllSequences(this);

	FStrifeDialogueNode *node;

	while (StrifeDialogues.Pop (node))
	{
		delete node;
	}

	DialogueRoots.Clear();
	ClassRoots.Clear();

	for (auto &sub : subsectors)
	{
		if (sub.BSP != nullptr) delete sub.BSP;
	}
	ClearPortals();

	tagManager.Clear();
	ClearTIDHashes();
	if (SpotState) SpotState->Destroy();
	SpotState = nullptr;
	ACSThinker = nullptr;
	ClientSideACSThinker = nullptr;
	FraggleScriptThinker = nullptr;
	CorpseQueue.Clear();
	canvasTextureInfo.EmptyList();
	sections.Clear();
	segs.Clear();
	extsectors.Clear();
	sectors.Clear();
	linebuffer.Clear();
	subsectorbuffer.Clear();
	lines.Clear();
	sides.Clear();
	segbuffer.Clear();
	loadsectors.Clear();
	loadlines.Clear();
	loadsides.Clear();
	vertexes.Clear();
	nodes.Clear();
	gamenodes.Reset();
	subsectors.Clear();
	gamesubsectors.Reset();
	rejectmatrix.Clear();
	Zones.Clear();
	blockmap.Clear();
	Polyobjects.Clear();

	for (auto &pb : PolyBlockMap)
	{
		polyblock_t *link = pb;
		while (link != nullptr)
		{
			polyblock_t *next = link->next;
			delete link;
			link = next;
		}
	}
	PolyBlockMap.Reset();

	deathmatchstarts.Clear();
	AllPlayerStarts.Clear();
	memset(playerstarts, 0, sizeof(playerstarts));
	Scrolls.Clear();
	if (automap) automap->Destroy();
	Behaviors.UnloadModules();
	localEventManager->Shutdown();

	if (aabbTree) delete aabbTree;
	if (levelMesh) delete levelMesh;
	aabbTree = nullptr;
	levelMesh = nullptr;
	VisualThinkerHead = nullptr;
	ActorBehaviors.Clear();
	ClientSideActorBehaviors.Clear();
	if (screen)
		screen->SetAABBTree(nullptr);
}

//==========================================================================
//
//
//
//==========================================================================

void P_FreeLevelData (bool fullgc)
{
	R_FreePastViewers();

	for (auto Level : AllLevels())
	{
		Level->ClearLevelData(fullgc);
	}
	// primaryLevel->FreeSecondaryLevels();
}

//===========================================================================
//
// P_SetupLevel
//
// [RH] position indicates the start spot to spawn at
//
//===========================================================================

void P_SetupLevel(FLevelLocals *Level, int position, bool newGame)
{
	unsigned int i;

	Level->ShaderStartTime = I_msTimeFS(); // indicate to the shader system that the level just started

	// This is motivated as follows:

	Level->maptype = MAPTYPE_UNKNOWN;

	if (!savegamerestore)
	{
		Level->SetMusicVolume(Level->MusicVolume);
		for (i = 0; i < MAXPLAYERS; ++i)
		{
			Level->Players[i]->killcount = Level->Players[i]->secretcount
				= Level->Players[i]->itemcount = 0;
		}
	}
	for (i = 0; i < MAXPLAYERS; ++i)
	{
		Level->Players[i]->mo = nullptr;
	}
	GPalette.ClearTranslationSlot(TRANSLATION_LevelScripted);


	// Initial height of PointOfView will be set by player think.
	auto p = Level->GetConsolePlayer();
	if (p) p->viewz = NO_VALUE;

	// Make sure all sounds are stopped before Z_FreeTags.
	S_Start();
	S_ResetMusic();

	// Don't start the music if loading a savegame, because the music is stored there.
	// Don't start the music if revisiting a level in a hub for the same reason.
	if (!primaryLevel->IsReentering())
	{
		primaryLevel->SetMusic();
	}

	// [RH] clear out the mid-screen message
	C_MidPrint(nullptr, nullptr);

	// Free all level data from the previous map
	P_FreeLevelData();

	MapData *map = P_OpenMapData(Level->MapName.GetChars(), true);
	if (map == nullptr)
	{
		I_Error("Unable to open map '%s'\n", Level->MapName.GetChars());
	}

	// [ZZ] init per-map static handlers. we need to call this before everything is set up because otherwise scripts don't receive PlayerEntered event
	//      (which happens at god-knows-what stage in this function, but definitely not the last part, because otherwise it'd work to put E_InitStaticHandlers before the player spawning)
	Level->localEventManager->InitStaticHandlers(Level, true);

	// generate a checksum for the level, to be included and checked with savegames.
	map->GetChecksum(Level->md5);
	// find map num
	Level->lumpnum = map->lumpnum;

	if (newGame)
	{
		Level->localEventManager->NewGame();
	}

	MapLoader loader(Level);
	loader.LoadLevel(map, Level->MapName.GetChars(), position);
	delete map;

	// if deathmatch, randomly spawn the active players
	if (deathmatch)
	{
		for (i = 0; i < MAXPLAYERS; i++)
		{
			if (Level->PlayerInGame(i))
			{
				Level->Players[i]->mo = nullptr;
				Level->DeathMatchSpawnPlayer(i);
			}
		}
	}
	// the same, but for random single/coop player starts
	else if (Level->flags2 & LEVEL2_RANDOMPLAYERSTARTS)
	{
		for (i = 0; i < MAXPLAYERS; ++i)
		{
			if (Level->PlayerInGame(i))
			{
				Level->Players[i]->mo = nullptr;
				FPlayerStart *mthing = Level->PickPlayerStart(i);
				Level->SpawnPlayer(mthing, i, (Level->flags2 & LEVEL2_PRERAISEWEAPON) ? SPF_WEAPONFULLYUP : 0);
			}
		}
	}
	else if (newGame)
	{
		for (i = 0; i < MAXPLAYERS; ++i)
		{
			// Didn't have a player spawn available so spawn it now.
			if (Level->PlayerInGame(i) && Level->Players[i]->playerstate == PST_ENTER && Level->Players[i]->mo == nullptr)
			{
				FPlayerStart* mthing = Level->PickPlayerStart(i);
				Level->SpawnPlayer(mthing, i, (Level->flags2 & LEVEL2_PRERAISEWEAPON) ? SPF_WEAPONFULLYUP : 0);
			}
		}
	}

	// [SP] move unfriendly players around
	// horribly hacky - yes, this needs rewritten.
	if (Level->deathmatchstarts.Size() > 0)
	{
		for (i = 0; i < MAXPLAYERS; ++i)
		{
			auto p = Level->Players[i];
			if (Level->PlayerInGame(i) && p->mo != nullptr)
			{
				if (!(p->mo->flags & MF_FRIENDLY))
				{
					AActor * oldSpawn = p->mo;
					Level->DeathMatchSpawnPlayer(i);
					oldSpawn->Destroy();
				}
			}
		}
	}


	// Don't count monsters in end-of-level sectors if option is on
	if (dmflags2 & DF2_NOCOUNTENDMONST)
	{
		auto it = Level->GetThinkerIterator<AActor>();
		AActor * mo;

		while ((mo = it.Next()))
		{
			if (mo->flags & MF_COUNTKILL)
			{
				if (mo->Sector->damageamount > 0 && (mo->Sector->Flags & (SECF_ENDGODMODE | SECF_ENDLEVEL)) == (SECF_ENDGODMODE | SECF_ENDLEVEL))
				{
					mo->ClearCounters();
				}
			}
		}
	}

	T_PreprocessScripts(Level);        // preprocess FraggleScript scripts

	// build subsector connect matrix
	//	UNUSED P_ConnectSubsectors ();

	R_OldBlend = 0xffffffff;

	// [RH] Remove all particles
	P_ClearParticles(Level);

	// Background asset loader (gl_texture_thread): finish loads the previous level queued before this level's precache
	// rebuilds texture state, so no worker still holds a texture (GZSelaco p_setup.cpp). A no-op when the loader is off.
	if (screen != nullptr && screen->SupportsBackgroundCache())
	{
		screen->FlushBackground();
	}

	// preload graphics and sounds
	if (precache)
	{
		PrecacheLevel(Level);
		S_PrecacheLevel(Level);
	}

	if (deathmatch)
	{
		AnnounceGameStart();
	}

	//Trigger a Yaw/Pitch recalc in the QuestZDoom code to avoid moving in the wrong direction
	resetDoomYaw = true;
	resetPreviousPitch = true;
	VR_ResetTransientNetSafeState();

	// This check was previously done at run time each time the heightsec was checked.
	// However, since 3D floors are static data, we can easily precalculate this and store it in the sector's flags for quick access.
	for (auto &s : Level->sectors)
	{
		if (s.heightsec != nullptr)
		{
			// If any of these 3D floors render their planes, ignore heightsec.
			for (auto &ff : s.e->XFloor.ffloors)
			{
				if ((ff->flags & (FF_EXISTS | FF_RENDERPLANES)) == (FF_EXISTS | FF_RENDERPLANES))
				{
					s.MoreFlags |= SECMF_IGNOREHEIGHTSEC;	// mark the heightsec inactive.
				}
			}
		}
	}

	// Create a backup of the map data so the savegame code can toss out all fields that haven't changed in order to reduce processing time and file size.
	// Note that we want binary identity here, so assignment is not sufficient because it won't initialize any padding bytes.
	// Note that none of these structures may contain non POD fields anyway.
	Level->loadsectors.Resize(Level->sectors.Size());
	memcpy(&Level->loadsectors[0], &Level->sectors[0], Level->sectors.Size() * sizeof(Level->sectors[0]));
	Level->loadlines.Resize(Level->lines.Size());
	memcpy(&Level->loadlines[0], &Level->lines[0], Level->lines.Size() * sizeof(Level->lines[0]));
	Level->loadsides.Resize(Level->sides.Size());
	memcpy(&Level->loadsides[0], &Level->sides[0], Level->sides.Size() * sizeof(Level->sides[0]));

	Level->automap = AM_Create(Level);
	Level->automap->LevelInit();
	Level->SetCompatLineOnSide(true);

	// [RH] Start lightning, if MAPINFO tells us to
	if (Level->flags & LEVEL_STARTLIGHTNING)
	{
		Level->StartLightning();
	}

	auto it = Level->GetThinkerIterator<AActor>();
	AActor* ac;

	Level->flags3 |= LEVEL3_LIGHTCREATED;

	// Initial setup of the dynamic lights.
	while ((ac = it.Next()))
	{
		ac->SetDynamicLights();
	}
}

//
// P_Init
//
void P_Init ()
{
	P_InitEffects ();		// [RH]
	P_InitTerrainTypes ();
	P_InitKeyMessages ();
	R_InitSprites ();
}

void P_Shutdown ()
{
	for (auto Level : AllLevels())
	{
		Level->Thinkers.DestroyThinkersInList(STAT_STATIC);
		Level->ClientSideThinkers.DestroyThinkersInList(STAT_STATIC);
	}
	P_FreeLevelData ();
	// [ZZ] delete global event handlers
	staticEventManager.Shutdown();	// clear out the handlers before starting the engine shutdown
	ST_Clear();
	for (auto &p : players)
	{
		if (p.psprites != nullptr) p.psprites->Destroy();
	}
}

//==========================================================================
//
// dumpgeometry
//
//==========================================================================

CCMD(dumpgeometry)
{
	for (auto Level : AllLevels())
	{
		Printf("Geometry for %s\n", Level->MapName.GetChars());
		for (auto &sector : Level->sectors)
		{
			Printf(PRINT_LOG, "Sector %d\n", sector.sectornum);
			for (int j = 0; j<sector.subsectorcount; j++)
			{
				subsector_t * sub = sector.subsectors[j];

				Printf(PRINT_LOG, "    Subsector %d - real sector = %d - %s\n", int(sub->Index()), sub->sector->sectornum, sub->hacked & 1 ? "hacked" : "");
				for (uint32_t k = 0; k<sub->numlines; k++)
				{
					seg_t * seg = sub->firstline + k;
					if (seg->linedef)
					{
						Printf(PRINT_LOG, "      (%4.4f, %4.4f), (%4.4f, %4.4f) - seg %d, linedef %d, side %d",
							seg->v1->fX(), seg->v1->fY(), seg->v2->fX(), seg->v2->fY(),
							seg->Index(), seg->linedef->Index(), seg->sidedef != seg->linedef->sidedef[0]);
					}
					else
					{
						Printf(PRINT_LOG, "      (%4.4f, %4.4f), (%4.4f, %4.4f) - seg %d, miniseg",
							seg->v1->fX(), seg->v1->fY(), seg->v2->fX(), seg->v2->fY(), seg->Index());
					}
					if (seg->PartnerSeg)
					{
						subsector_t * sub2 = seg->PartnerSeg->Subsector;
						Printf(PRINT_LOG, ", back sector = %d, real back sector = %d", sub2->render_sector->sectornum, seg->PartnerSeg->frontsector->sectornum);
					}
					else if (seg->backsector)
					{
						Printf(PRINT_LOG, ", back sector = %d (no partnerseg)", seg->backsector->sectornum);
					}

					Printf(PRINT_LOG, "\n");
				}
			}
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

CCMD(listmapsections)
{
	for (auto Level : AllLevels())
	{
		Printf("Map sections for %s:\n", Level->MapName.GetChars());
		for (int i = 0; i < 100; i++)
		{
			for (auto &sub : Level->subsectors)
			{
				if (sub.mapsection == i)
				{
					Printf("Mapsection %d, sector %d, line %d\n", i, sub.render_sector->Index(), sub.firstline->linedef->Index());
					break;
				}
			}
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

CUSTOM_CVAR(Bool, forcewater, false, CVAR_ARCHIVE | CVAR_SERVERINFO)
{
	if (gamestate == GS_LEVEL) for (auto Level : AllLevels())
	{
		for (auto &sec : Level->sectors)
		{
			sector_t *hsec = sec.GetHeightSec();
			if (hsec && !(hsec->MoreFlags & SECMF_UNDERWATER))
			{
				if (self)
				{
					hsec->MoreFlags |= SECMF_FORCEDUNDERWATER;
				}
				else
				{
					hsec->MoreFlags &= ~SECMF_FORCEDUNDERWATER;
				}
			}
		}
	}
}
