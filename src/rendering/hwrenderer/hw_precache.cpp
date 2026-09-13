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

		for (unsigned i = 0; i < cls->GetStateCount(); i++)
		{
			auto &state = cls->GetStates()[i];
			spritelist[state.sprite].Insert(gltrans, true);
			FSpriteModelFrame * smf = FindModelFrame(cls, state.sprite, state.Frame, false);

			// RS fork -- r_voxels_mode 1, see precacheOverrideVoxels. A voxel frame is
			// always one model with its palette as the skin (InitModels, models.cpp).
			if (precacheOverrideVoxels)
			{
				FSpriteModelFrame *vox = FindVoxelFrame(state.sprite, state.Frame, false);
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
			}
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
}
