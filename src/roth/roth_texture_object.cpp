/*
** roth_texture_object.cpp
**
** TextureSet::Sprite -- the registration path for OBJECT art, as opposed to
** world-surface art.
**
** WHY THIS IS A SEPARATE FILE. It is one method of roth_texture.cpp's class and
** belongs in roth_texture.cpp; it lives here only because that file was being
** edited by another lane when object spawning was written. Merge it back when
** that lane closes. Nothing else should accumulate here in the meantime.
**
** OBJECT ART IS STORED ROTATED, exactly like wall art -- see the long comment on
** TextureSet::Sprite in roth_texture.h for the two pieces of evidence, one from
** ROTH.C's arithmetic and one from looking at the decoded retail pictures. The
** handoff says the opposite; the handoff is wrong. So the quarter turn is undone
** here the same free way it is for walls: exchange the dimensions and declare
** the stored bytes column-major.
**
** What IS different from a wall: object art is masked by default (a prop is a
** cut-out), it carries sprite offsets so the picture stands on the Z the map
** gave it, and its drawn size can be scaled by the artwork's own modifier.
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include "roth_texture.h"
#include "roth_das.h"
#include "roth_log.h"

#include "bitmap.h"
#include "textures.h"
#include "image.h"
#include "animations.h"
#include "texturemanager.h"
#include "gametexture.h"
#include "palettecontainer.h"
#include "r_data/r_translate.h"

namespace roth
{

//==========================================================================
//
// Decoded pixels the object textures point into.
//
// A DUPLICATE of roth_texture.cpp's KeepPixels, for the same reason this file
// exists: that one is static to its translation unit and that file is not mine
// to change right now. Fold the two together when this file is folded back in;
// there should be one pixel store, not two.
//
// Never released, like the other one: FImageSource lives in ImageArena and is
// freed in bulk without its destructor running, so an image cannot own its
// pixels, and a texture can be drawn at any time after a load.
//
//==========================================================================

static std::vector<std::vector<uint8_t> *> ObjectPixelStore;

static const uint8_t *KeepObjectPixels(const std::vector<uint8_t> &src)
{
	auto *block = new std::vector<uint8_t>(src);
	ObjectPixelStore.push_back(block);
	return block->data();
}

// Matches roth_texture.cpp's ANIM_FRAME_MS, and carries the same caveat: the
// rate came from the Python pipeline looking right, not from ROTH.C.
static const int OBJECT_ANIM_FRAME_MS = 229;

//==========================================================================
//
// Names. A separate name space from the world path's, because the same stored
// index can be registered both ways and the name is what the manager keys on.
//
//==========================================================================

static FString ObjectName(const std::string &pack, int index)
{
	return FStringf("ROTH_%s_O%05d", pack.c_str(), index);
}

static FString ObjectFrameName(const std::string &pack, int index, int frame)
{
	return FStringf("ROTH_%s_O%05d_%02d", pack.c_str(), index, frame);
}

//==========================================================================
//
// How big one texel is drawn, and where the picture hangs off the object's z.
//
// renderer.c:6512-6525 for the size modifier, 6549-6561 for the anchoring. Both
// are properties of the ARTWORK, not of the object that uses it, which is why
// they are worked out here and cached per stored index.
//
//==========================================================================

static void ReadSpriteModifiers(const Image &img, SpriteInfo &out)
{
	// The word ROTH.C tests is (modifier | image_type << 8). Bit 0x80 of the
	// modifier says a size modifier is present; bits 0x2000/0x4000 of the word
	// -- image_type bits 5 and 6 -- are its exponent.
	if (img.modifier & IM_HALF_SIZE)
	{
		const int s = (img.imageType >> 5) & 3;
		out.unitsPerPixel = (s == 0) ? 1.0 : 2.0 * double(1 << s);
	}
	out.hang = (img.modifier & IM_HANG) != 0;
	out.anchorShift = 2.0 * double(img.modifier & 0x0f);
}

//==========================================================================
//
//
//
//==========================================================================

FTextureID TextureSet::Sprite(int index, Log *log, SpriteInfo *info)
{
	SpriteInfo local;
	if (info == nullptr) info = &local;
	*info = SpriteInfo();

	if (!mPack) { info->failed = true; return FNullTextureID(); }

	auto cachedInfo = mSpriteInfo.find(index);
	auto found = mByIndexSprite.find(index);
	if (found != mByIndexSprite.end() && cachedInfo != mSpriteInfo.end())
	{
		// Only trust the memo while the texture manager still agrees; it can be
		// rebuilt underneath us, which invalidates every id handed out.
		if (!found->second.isValid() ||
			TexMan.GetGameTexture(found->second, false) != nullptr)
		{
			*info = cachedInfo->second;
			return found->second;
		}
		mByIndexSprite.erase(found);
		mSpriteInfo.erase(cachedInfo);
	}

	FTextureID id;

	// The colour sentinels, resolved exactly as World does: nearest base first,
	// or every high-cluster index reads against the wrong base. ROTH.C's own
	// test is looser than either base -- renderer.c:13103 takes any id with bit
	// 0x8000 set and uses its low byte as the colour -- and both bases satisfy
	// that, so the two agree on every value the retail data actually contains.
	// A solid colour has no orientation, so the world path's registration is
	// reused rather than duplicated.
	if (index >= (int)TEX_COLOUR_BASE_HI)
		id = SolidColour(index - (int)TEX_COLOUR_BASE_HI, log);
	else if (index >= (int)TEX_COLOUR_BASE_LO)
		id = SolidColour(index - (int)TEX_COLOUR_BASE_LO, log);
	else if (index >= (int)TEX_PICTURE_LIMIT)
	{
		// Past where this pack's pictures stop and matching neither colour base.
		// 3D mesh faces DO reach here in the retail data -- DEMO's mesh 4097
		// textures every face with 9600 -- so unlike the wall path this is not
		// necessarily a modified map. ROTH.C reads ids at or above 0x1200 from
		// the SHARED pack handle instead (das_assets.c:919), which this does not
		// implement, so it is reported rather than guessed at.
		mOutOfRange++;
		if (log) log->Count("objects: art index above the picture limit (shared-pack id?)");
		id = SolidColour(0, log);
	}
	else
	{
		FString name = ObjectName(mName, index);
		FTextureID existing = TexMan.CheckForTexture(name.GetChars(), ETextureType::Override);
		Image img = mPack->ReadImage(index, true);
		if (!img.ok())
		{
			mSpritesFailed++;
			info->failed = true;
			if (log) log->Count("objects: art could not be decoded");
			mByIndexSprite[index] = FNullTextureID();
			mSpriteInfo[index] = *info;
			return FNullTextureID();
		}
		ReadSpriteModifiers(img, *info);

		if (existing.isValid())
		{
			mByIndexSprite[index] = existing;
			mSpriteInfo[index] = *info;
			return existing;
		}

		// Index 0 is a hole: a prop is a cut-out. An IT_TRANSLUCENT picture
		// additionally blends its high palette half, which is the world path's
		// third translation.
		const bool translucent = (img.imageType & IT_TRANSLUCENT) != 0;
		FRemapTable *remap = GPalette.GetTranslation(TRANSLATION_Standard,
			translucent && mTranslucentTranslation >= 0 ? mTranslucentTranslation
			: mTranslation);
		if (translucent && log) log->Count("objects: translucent art (IT_TRANSLUCENT)");

		// The quarter turn, free: exchange the dimensions and hand the stored
		// bytes over as column-major. Same as a wall; see the file comment.
		const int w = img.height;
		const int h = img.width;

		// Where the picture sits relative to the actor's z. GZDoom draws a
		// sprite with its TOP at z + topoffset, so a top offset of the full
		// height puts the bottom on z -- a prop standing on the floor the map
		// gave it. An IM_HANG picture is anchored by its top, which is offset
		// zero. The modifier's extra nibble shift is in WORLD units and depends
		// on the drawn scale, so it is applied by the caller at spawn, not here.
		auto addFrame = [&](const std::vector<uint8_t> &pixels, const char *texName) -> FTextureID
		{
			auto *image = new FPalettedMemoryImage(KeepObjectPixels(pixels), remap, w, h,
				true, true);
			auto *tex = MakeGameTexture(new FImageTexture(image), texName,
				ETextureType::Override);
			tex->SetOffsets(w / 2, info->hang ? 0 : h);
			tex->SetOffsetsNotForFont();
			return TexMan.AddGameTexture(tex);
		};

		id = addFrame(img.frames[0], name.GetChars());
		mSpritesRegistered++;

		// Frames land on a contiguous run of ids straight after the base, which
		// is what a simple forward animation wants.
		const int frames = (int)img.frames.size();
		if (frames > 1 && id.isValid())
		{
			for (int f = 1; f < frames; f++)
			{
				addFrame(img.frames[f], ObjectFrameName(mName, index, f).GetChars());
				mSpritesRegistered++;
			}
			TexAnim.AddSimpleAnim(id, frames, OBJECT_ANIM_FRAME_MS);
			if (log) log->Count("objects: animated art");
		}
	}

	mByIndexSprite[index] = id;
	mSpriteInfo[index] = *info;
	return id;
}

} // namespace roth
