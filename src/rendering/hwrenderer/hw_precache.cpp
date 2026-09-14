/*
** hw_precache.cpp
**
** Texture precaching
**
**---------------------------------------------------------------------------
**
** Copyright 2004-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "c_cvars.h"
#include "filesystem.h"
#include "r_data/r_translate.h"
#include "c_dispatch.h"
#include "r_state.h"
#include "actor.h"
#include "models.h"
#include "skyboxtexture.h"
#include "hw_material.h"
#include "image.h"
#include "v_video.h"
#include "v_font.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "hw_models.h"
#include "d_main.h"

EXTERN_CVAR(Bool, gl_precache)
EXTERN_CVAR(Bool, gl_texture_thread)
EXTERN_CVAR(Bool, gl_texture_thread_models)
EXTERN_CVAR(Bool, debug_precache_actor)	// [SELACO PRECACHE] hw_cvars.cpp: full state walk for every class, labels ignored

//==========================================================================
//
// DFrameBuffer :: PrecacheTexture
//
//==========================================================================

static void PrecacheTexture(FGameTexture *tex, int cache)
{
	if (cache & (FTextureManager::HIT_Wall | FTextureManager::HIT_Flat | FTextureManager::HIT_Sky))
	{
		int scaleflags = 0;
		if (shouldUpscale(tex, UF_Texture)) scaleflags |= CTF_Upscale;

		FMaterial * gltex = FMaterial::ValidateTexture(tex, scaleflags);
		if (gltex) screen->PrecacheMaterial(gltex, 0);
	}
}

//===========================================================================
//
//
//
//===========================================================================
static void PrecacheList(FMaterial *gltex, SpriteHits& translations)
{
	SpriteHits::Iterator it(translations);
	SpriteHits::Pair* pair;
	while (it.NextPair(pair))
	{
		if (gl_texture_thread && screen->SupportsBackgroundCache())
		{
			screen->PrequeueMaterial(gltex, pair->Key);
		}
		else
		{
			screen->PrecacheMaterial(gltex, pair->Key);
		}
	}
}

//==========================================================================
//
// DFrameBuffer :: PrecacheSprite
//
//==========================================================================

static void PrecacheSprite(FGameTexture *tex, SpriteHits &hits)
{
	int scaleflags = CTF_Expand;
	if (shouldUpscale(tex, UF_Sprite)) scaleflags |= CTF_Upscale;

	FMaterial * gltex = FMaterial::ValidateTexture(tex, scaleflags);
	if (gltex) PrecacheList(gltex, hits);
}

//==========================================================================
//
// MarkModelFramePrecache
//
// [SELACO PRECACHE] The model data and skins one class draws for one sprite
// frame. This was the body of hw_PrecacheTexture's per-state loop, unchanged,
// lifted out so a frame listed under a Precache: label marks its model
// exactly as a state of a level actor does. cls is the class the frame is
// drawn for, which is what MODELDEF is looked up by, even when the state
// itself was inherited from a parent class.
//
//==========================================================================

static void MarkModelFramePrecache(PClassActor *cls, int sprite, int frame, uint8_t *texhitlist, uint8_t *modellist, bool precacheOverrideVoxels)
{
	FSpriteModelFrame * smf = FindModelFrame(cls, sprite, frame, false);

	// RS fork -- r_voxels_mode 1, see precacheOverrideVoxels. A voxel frame is
	// always one model with its palette as the skin (InitModels, models.cpp).
	if (precacheOverrideVoxels)
	{
		FSpriteModelFrame *vox = FindVoxelFrame(sprite, frame, false);
		if (vox != nullptr && vox != smf && vox->modelsAmount > 0 && vox->modelIDs[0] != -1)
		{
			if (vox->skinIDs[0].isValid()) texhitlist[vox->skinIDs[0].GetIndex()] |= FTextureManager::HIT_Flat;
			modellist[vox->modelIDs[0]] = 1;
		}
	}
	if (smf != NULL)
	{
		for (int i = 0; i < smf->modelsAmount; i++)
		{
			if (smf->skinIDs[i].isValid())
			{
				texhitlist[smf->skinIDs[i].GetIndex()] |= FTextureManager::HIT_Flat;
			}
			else if (smf->modelIDs[i] != -1)
			{
				Models[smf->modelIDs[i]]->AddSkins(texhitlist, (unsigned)(i * MD3_MAX_SURFACES) < smf->surfaceskinIDs.Size()? &smf->surfaceskinIDs[i * MD3_MAX_SURFACES] : nullptr);
			}
			if (smf->modelIDs[i] != -1)
			{
				modellist[smf->modelIDs[i]] = 1;
			}
		}
	}
}


//==========================================================================
//
// DFrameBuffer :: Precache
//
//==========================================================================

void hw_PrecacheTexture(uint8_t *texhitlist, TMap<PClassActor*, bool> &actorhitlist)
{
	TMap<FTexture*, bool> allTextures;
	TArray<FTexture*> layers;

	// First collect the potential max. texture set
	for (int i = 1; i < TexMan.NumTextures(); i++)
	{
		auto gametex = TexMan.GameByIndex(i);
		if (gametex && gametex->isValid() &&
			gametex->GetTexture()->GetImage() &&	// only image textures are subject to precaching
			gametex->GetUseType() != ETextureType::FontChar &&	// We do not want to delete font characters here as they are very likely to be needed constantly.
			gametex->GetUseType() < ETextureType::Special)		// Any texture marked as 'special' is also out.
		{
			gametex->GetLayers(layers);
			for (auto layer : layers)
			{
				allTextures.Insert(layer, true);
				layer->CleanPrecacheMarker();
			}
		}

		// Mark the faces of a skybox as used.
		// This isn't done by the main code so it needs to be done here.
		// MBF sky transfers are being checked by the calling code to add HIT_Sky for them.
		if (texhitlist[i] & (FTextureManager::HIT_Sky))
		{
			auto tex = TexMan.GameByIndex(i);
			auto sb = dynamic_cast<FSkyBox*>(tex->GetTexture());
			if (sb)
			{
				for (int i = 0; i < 6; i++)
				{
					if (sb->faces[i])
					{
						int index = sb->faces[i]->GetID().GetIndex();
						texhitlist[index] |= FTextureManager::HIT_Flat;
					}
				}
			}
		}
	}

	SpriteHits *spritelist = new SpriteHits[sprites.Size()];
	SpriteHits **spritehitlist = new SpriteHits*[TexMan.NumTextures()];
	TMap<PClassActor*, bool>::Iterator it(actorhitlist);
	TMap<PClassActor*, bool>::Pair *pair;
	uint8_t *modellist = new uint8_t[Models.Size()];
	memset(modellist, 0, Models.Size());
	memset(spritehitlist, 0, sizeof(SpriteHits**) * TexMan.NumTextures());

	// [SELACO PRECACHE] What the PRECACHEALWAYS flag and the Precache: state label reached, so
	// that only their sprites get their sprite positioning worked out ahead of the first draw
	// ("SPRITE POSITIONING", further down). spritewanted is per sprite (all of its frames),
	// spiwanted per texture. Nothing else sets them: other actors precache exactly as before.
	uint8_t *spritewanted = new uint8_t[sprites.Size()];
	uint8_t *spiwanted = new uint8_t[TexMan.NumTextures()];
	memset(spritewanted, 0, sprites.Size());
	memset(spiwanted, 0, TexMan.NumTextures());

	// [SELACO PRECACHE] Whether a frame left out of a Precache: label can arrive later without
	// stalling a frame: the texture thread switched on AND a backend running it (Vulkan). The
	// on-demand background loads in hw_sprites.cpp and hw_weapon.cpp are gated on the same two.
	const bool backgroundLoader = gl_texture_thread && screen->SupportsBackgroundCache();

	// RS FORK -- r_voxels_mode 1 (VoxelOverride only). The lookup below is the
	// gated one and refuses voxels in this mode, so without this every voxel
	// model stays unmarked, loses its vertex buffer further down, and the first
	// grab of each rebuilds it mid-frame. Marked anyway: VRAM for the pack, not
	// frame time. Modes 0 and 2 need nothing (0 finds them; 2 never draws them).
	const bool precacheOverrideVoxels = VoxelsEffectiveMode() == 1;

	// Check all used actors.
	// 1. mark all sprites associated with its states
	// 2. mark all model data and skins associated with its states
	while (it.NextPair(pair))
	{
		PClassActor *cls = pair->Key;
		auto remap = GPalette.TranslationToTable(GetDefaultByType(cls)->Translation.index());
		int gltrans = remap == nullptr ? 0 : remap->Index;

		// [SELACO PRECACHE] A Precache: label (Selaco's name; labels are inherited). With the
		// background loader it REPLACES this class's state walk: only the frames it lists load, in
		// the label walk further down, and anything else the class draws comes through the loader
		// on first use. Without the loader a frame left off the list would load synchronously
		// mid-play, so the full walk stays. debug_precache_actor forces the full walk.
		const bool precacheLabel = cls->FindStateByString("precache", true) != nullptr;
		if (precacheLabel && backgroundLoader && !debug_precache_actor) continue;

		// [SELACO PRECACHE] PRECACHEALWAYS or a label also walks the states the class inherits.
		// GetStates() holds only the states a class declares itself, so a subclass that restyles its
		// parent -- RSB_Rocket : Rocket, the casing classes under RSB_LocalEjecta -- would mark
		// nothing, and its MODELDEF model is found only by asking with the subclass. So each state is
		// looked up with cls, not with the class that declared it. Stops before Actor's own generic
		// states. A class with neither walks only its own states, as before.
		const bool walkInherited = precacheLabel || (GetDefaultByType(cls)->flags9 & MF9_PRECACHEALWAYS);
		for (PClassActor *owner = cls; owner != nullptr; )
		{
			for (unsigned i = 0; i < owner->GetStateCount(); i++)
			{
				auto &state = owner->GetStates()[i];
				spritelist[state.sprite].Insert(gltrans, true);
				if (walkInherited) spritewanted[state.sprite] = 1;
				MarkModelFramePrecache(cls, state.sprite, state.Frame, texhitlist, modellist, precacheOverrideVoxels);
			}
			if (!walkInherited || owner == RUNTIME_CLASS(AActor)) break;
			owner = static_cast<PClassActor *>(owner->ParentClass);
			if (owner == RUNTIME_CLASS(AActor)) break;
		}
	}

	// mark all sprite textures belonging to the marked sprites.
	for (int i = (int)(sprites.Size() - 1); i >= 0; i--)
	{
		if (spritelist[i].CountUsed())
		{
			int j, k;
			for (j = 0; j < sprites[i].numframes; j++)
			{
				const spriteframe_t *frame = &SpriteFrames[sprites[i].spriteframes + j];

				for (k = 0; k < 16; k++)
				{
					FTextureID pic = frame->Texture[k];
					if (pic.isValid())
					{
						spritehitlist[pic.GetIndex()] = &spritelist[i];
					}
				}
			}
		}
	}

	// [SELACO PRECACHE] THE PRECACHE: LABEL WALK (GZSelaco 863a34c9b2, ba7545688e), with the
	// background loader only (see the class walk above). Frame by frame from the label to its Stop:
	// each listed frame's textures, all rotations, and its model. It runs after the sprite marking
	// above, which marks every frame of a marked sprite; here only the listed frames are marked, so
	// "Precache: BAL1 A 0; Stop;" loads BAL1A and not the rest of BAL1. The frames may be any
	// sprite, not only the class's own, so a weapon can list the effects it will spawn.
	// "Precache: TNT1 A 0; Stop;" lists nothing, so the class loads nothing: Selaco's opt-out.
	// Differences from Selaco: a TNT1 frame is skipped on its own (Selaco dropped the whole class
	// when TNT1 came first), and a label that loops or jumps back stops quietly at the first
	// repeated state. Selaco's 1000-state cap stays, reported in red.
	if (backgroundLoader)
	{
		static const int PRECACHE_STATE_LIMIT = 1000;
		it.Reset();
		while (it.NextPair(pair))
		{
			PClassActor *cls = pair->Key;
			FState *state = cls->FindStateByString("precache", true);
			if (state == nullptr) continue;

			auto remap = GPalette.TranslationToTable(GetDefaultByType(cls)->Translation.index());
			int gltrans = remap == nullptr ? 0 : remap->Index;
			TMap<FState *, bool> walked;
			int count = 0;
			for (; state != nullptr && walked.CheckKey(state) == nullptr; state = state->GetNextState())
			{
				if (++count > PRECACHE_STATE_LIMIT)
				{
					Printf(TEXTCOLOR_RED "Pre-Cache error: %d+ states encountered in %s!\n", PRECACHE_STATE_LIMIT, cls->TypeName.GetChars());
					break;
				}
				walked.Insert(state, true);
				if (state->sprite == 0) continue;	// TNT1 (sprite 0 always is): nothing to draw

				spritelist[state->sprite].Insert(gltrans, true);
				// The frame index is checked, as Selaco does, in case a state names a frame its sprite lacks.
				if ((unsigned)state->Frame < (unsigned)sprites[state->sprite].numframes)
				{
					const spriteframe_t *frame = &SpriteFrames[sprites[state->sprite].spriteframes + state->Frame];
					for (int k = 0; k < 16; k++)
					{
						FTextureID pic = frame->Texture[k];
						if (pic.isValid())
						{
							spritehitlist[pic.GetIndex()] = &spritelist[state->sprite];
							spiwanted[pic.GetIndex()] = 1;
						}
					}
				}
				MarkModelFramePrecache(cls, state->sprite, state->Frame, texhitlist, modellist, precacheOverrideVoxels);
			}
		}
	}

	// [SELACO PRECACHE] Every frame of a sprite that the flag or label class walk reached wants its
	// sprite positioning ahead of its first draw ("SPRITE POSITIONING" below).
	for (unsigned i = 0; i < sprites.Size(); i++)
	{
		if (!spritewanted[i] || spritelist[i].CountUsed() == 0) continue;
		for (int j = 0; j < sprites[i].numframes; j++)
		{
			const spriteframe_t *frame = &SpriteFrames[sprites[i].spriteframes + j];
			for (int k = 0; k < 16; k++)
			{
				FTextureID pic = frame->Texture[k];
				if (pic.isValid()) spiwanted[pic.GetIndex()] = 1;
			}
		}
	}

	// delete everything unused before creating any new resources to avoid memory usage peaks.

	// delete unused models
	for (unsigned i = 0; i < Models.Size(); i++)
	{
		if (!modellist[i]) Models[i]->DestroyVertexBuffer();
	}

	TMap<FTexture *, bool> usedTextures, usedSprites;

	screen->StartPrecaching();
	int cnt = TexMan.NumTextures();

	// prepare the textures for precaching. First mark all used layer textures so that we know which ones should not be deleted.
	for (int i = cnt - 1; i >= 0; i--)
	{
		auto tex = TexMan.GameByIndex(i);
		if (tex != nullptr)
		{
			if (texhitlist[i] & (FTextureManager::HIT_Wall | FTextureManager::HIT_Flat | FTextureManager::HIT_Sky))
			{
				int scaleflags = 0;
				if (shouldUpscale(tex, UF_Texture)) scaleflags |= CTF_Upscale;

				FMaterial* mat = FMaterial::ValidateTexture(tex, scaleflags, true);
				if (mat != nullptr)
				{
					for (auto &layer : mat->GetLayerArray())
					{
						if (layer.layerTexture) layer.layerTexture->MarkForPrecache(0, layer.scaleFlags);
					}
				}
			}
			if (spritehitlist[i] != nullptr && (*spritehitlist[i]).CountUsed() > 0)
			{
				int scaleflags = CTF_Expand;
				if (shouldUpscale(tex, UF_Sprite)) scaleflags |= CTF_Upscale;

				FMaterial *mat = FMaterial::ValidateTexture(tex, true, true);
				if (mat != nullptr)
				{
					SpriteHits::Iterator it(*spritehitlist[i]);
					SpriteHits::Pair* pair;
					while (it.NextPair(pair))
					{
						for (auto& layer : mat->GetLayerArray())
						{
							if (layer.layerTexture) layer.layerTexture->MarkForPrecache(pair->Key, layer.scaleFlags);
						}
					}
				}
			}
		}
	}

	// delete unused hardware textures (i.e. those which didn't get referenced by any material in the cache list.)
	decltype(allTextures)::Iterator ita(allTextures);
	decltype(allTextures)::Pair* paira;
	while (ita.NextPair(paira))
	{
		paira->Key->CleanUnused();
	}

	if (gl_precache)
	{
		cycle_t precache;
		precache.Reset();
		precache.Clock();

		FImageSource::BeginPrecaching();

		// cache all used images
		for (int i = cnt - 1; i >= 0; i--)
		{
			auto gtex = TexMan.GameByIndex(i);
			auto tex = gtex->GetTexture();
			if (tex != nullptr && tex->GetImage() != nullptr)
			{
				if (texhitlist[i] & (FTextureManager::HIT_Wall | FTextureManager::HIT_Flat | FTextureManager::HIT_Sky))
				{
					int flags = shouldUpscale(gtex, UF_Texture);
					if (tex->GetImage() && tex->GetHardwareTexture(0, flags) == nullptr)
					{
						FImageSource::RegisterForPrecache(tex->GetImage(), V_IsTrueColor());
					}
				}

				// Only register untranslated sprite images. Translated ones are very unlikely to require data that can be reused so they can just be created on demand.
				if (spritehitlist[i] != nullptr && (*spritehitlist[i]).CheckKey(0))
				{
					FImageSource::RegisterForPrecache(tex->GetImage(), V_IsTrueColor());
				}

				// [SELACO PRECACHE] A second reference for the SPRITE POSITIONING pass below, so its trim
				// and the upload share one decode of the image (GetCachedBitmap keeps the pixels for the
				// second reader). Only an expanded sprite trims. True colour, because the trim reads the
				// BGRA path.
				if (spiwanted[i] && spritehitlist[i] != nullptr && gtex->ShouldExpandSprite())
				{
					FImageSource::RegisterForPrecache(tex->GetImage(), true);
				}
			}
		}

		// [SELACO PRECACHE] SPRITE POSITIONING (GZSelaco 3512871d99), for flag and label sprites only.
		// A sprite's first draw works out its trim rectangle, and for an expanded sprite that decodes
		// the whole image on the main thread (FGameTexture::SetupSpriteData -> TrimBorders): a stall
		// that precaching the texture does not remove. Done here instead, once per session per sprite
		// (it is kept on the game texture, not freed at level change). This runs before the upload
		// loop, so the main thread has finished reading the precache cache before any upload is queued
		// to the texture threads. Selaco later moved this into its loader (781eed8d59); ours does not
		// compute it.
		for (int i = cnt - 1; i >= 0; i--)
		{
			if (!spiwanted[i] || spritehitlist[i] == nullptr) continue;
			auto gtex = TexMan.GameByIndex(i);
			if (gtex != nullptr && gtex->isValid()) gtex->GetSpritePositioning(1);
		}

		// cache all used textures
		for (int i = cnt - 1; i >= 0; i--)
		{
			auto gtex = TexMan.GameByIndex(i);
			if (gtex != nullptr)
			{
				PrecacheTexture(gtex, texhitlist[i]);
				if (spritehitlist[i] != nullptr && (*spritehitlist[i]).CountUsed() > 0)
				{
					PrecacheSprite(gtex, *spritehitlist[i]);
				}
			}
		}

		if (gl_texture_thread && screen->SupportsBackgroundCache())
		{
			screen->FlushBackground();
		}

		FImageSource::EndPrecaching();

		// cache all used models
		FModelRenderer* renderer = new FHWModelRenderer(nullptr, *screen->RenderState(), -1);
		for (unsigned i = 0; i < Models.Size(); i++)
		{
			if (modellist[i] && gl_texture_thread && gl_texture_thread_models && screen->SupportsBackgroundCache())
			{
				screen->BackgroundLoadModel(Models[i]);
			}
		}

		if (gl_texture_thread && gl_texture_thread_models && screen->SupportsBackgroundCache())
		{
			screen->FlushBackground();
		}

		for (unsigned i = 0; i < Models.Size(); i++)
		{
			if (modellist[i])
			{
				Models[i]->BuildVertexBuffer(renderer);
			}
		}
		delete renderer;

		precache.Unclock();
		DPrintf(DMSG_NOTIFY, "Textures precached in %.3f ms\n", precache.TimeMS());
	}

	delete[] spritehitlist;
	delete[] spritelist;
	delete[] modellist;
	delete[] spritewanted;	// [SELACO PRECACHE]
	delete[] spiwanted;
}
