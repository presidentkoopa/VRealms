/*
** roth_texture.cpp
**
** Registers a Realms of the Haunting .DAS artwork pack with the texture
** manager, reading it straight out of the player's installation.
**
** Nothing is generated on disk. The pack file is read into memory, images are
** decoded into memory, and FPalettedMemoryImage borrows those buffers -- so this
** module keeps them alive for the process lifetime. Textures are added as
** ETextureType::Override, which is the one type that serves as both a wall
** texture and a flat, so a Realms index that is used for both needs one entry.
**
** THE QUARTER TURN IS FREE. Wall and flat art is stored rotated 90 degrees: the
** along-wall axis indexes stored ROWS. A DAS image's bytes are row-major with a
** stride of the stored width, and GZDoom's paletted pixels are column-major
** (x*Height + y). Writing out the transposed image's pixel (x, y) means reading
** stored[x*storedWidth + y], which IS the column-major index once width and
** height are exchanged. So the rotation is undone by swapping the dimensions and
** declaring the bytes column-major -- no pixel shuffling at all. Sprites and
** model skins are NOT stored rotated and will want columnMajor = false.
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include "roth_texture.h"
#include "roth_das.h"
#include "roth_install.h"
#include "roth_log.h"

#include "colormatcher.h"
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
// Decoded pixels the textures point into.
//
// FImageSource lives in ImageArena and is freed in bulk without its destructor
// running, so an image cannot own its pixels. These blocks are therefore never
// released: they are bounded by the size of the packs the player's install
// actually contains, and a texture can be drawn at any time after a load.
//
//==========================================================================

static std::vector<std::vector<uint8_t> *> PixelStore;

static const uint8_t *KeepPixels(const std::vector<uint8_t> &src)
{
	auto *block = new std::vector<uint8_t>(src);
	PixelStore.push_back(block);
	return block->data();
}

//==========================================================================
//
// How long one frame of an animated Realms image is shown.
//
// UNVERIFIED. The Python pipeline used 8 tics (~229 ms) and looked right, and
// that is the only evidence for this number -- it was not read out of ROTH.C.
// Treat it as a placeholder: the rate is a single constant here so that
// correcting it is a one-line change.
//
//==========================================================================

static const int ANIM_FRAME_MS = 229;

//==========================================================================
//
// Two translations per pack: one where stored index 0 is a hole and one where
// it is an ordinary colour. Realms decides that per image (IT_ZERO_OPAQUE), and
// transparency lives in the palette's alpha, so it cannot be one table.
//
//==========================================================================

// Cached per pack, because a pack's palette does not change between loads and
// the translation manager has no way to retire one. Without this, loading the
// same map twice would store a second identical pair.
static std::map<std::string, std::pair<int, int>> TranslationsByPack;

static int MakeTranslation(const std::vector<Colour> &palette, bool zeroTransparent)
{
	FRemapTable opal;
	for (int c = 0; c < 256; c++)
	{
		const Colour &col = palette[c];
		opal.Palette[c] = PalEntry(255, col.r, col.g, col.b);
		opal.Remap[c] = ColorMatcher.Pick(col.r, col.g, col.b);
	}
	if (zeroTransparent)
	{
		// Alpha 0 is what CopyPixelData tests to skip a pixel, and index 0 is
		// the paletted renderer's own transparent entry.
		opal.Palette[0] = 0;
		opal.Remap[0] = 0;
	}
	return GetTranslationIndex(GPalette.StoreTranslation(TRANSLATION_Standard, &opal));
}

//==========================================================================
//
//
//
//==========================================================================

bool TextureSet::Open(const char *packName, const std::string &packFile, Log *log)
{
	mName = packName ? packName : "";
	for (auto &ch : mName) ch = (char)toupper((unsigned char)ch);

	mBytes = Install::ReadWholeFile(packFile);
	if (mBytes.empty())
	{
		mError = "could not read " + packFile;
		if (log) log->Warn("artwork: %s", mError.c_str());
		return false;
	}

	// The Pack borrows mBytes, which is why mBytes is a member and why nothing
	// may resize it after this point.
	mPack = new Pack();
	if (!mPack->Load(mBytes.data(), mBytes.size()))
	{
		mError = mPack->Error();
		if (log) log->Warn("artwork: pack %s: %s", mName.c_str(), mError.c_str());
		delete mPack;
		mPack = nullptr;
		return false;
	}

	auto cached = TranslationsByPack.find(mName);
	if (cached != TranslationsByPack.end())
	{
		mTranslation = cached->second.first;
		mOpaqueTranslation = cached->second.second;
	}
	else
	{
		mTranslation = MakeTranslation(mPack->Palette(), true);
		mOpaqueTranslation = MakeTranslation(mPack->Palette(), false);
		TranslationsByPack[mName] = { mTranslation, mOpaqueTranslation };
	}

	if (log)
	{
		log->Line("  pack %-8s entries %5d   sky index %5d",
			mName.c_str(), mPack->Count(), (int)mPack->SkyIndex());
	}
	return true;
}

uint16_t TextureSet::SkyIndex() const
{
	return mPack ? mPack->SkyIndex() : 0;
}

//==========================================================================
//
// The name a stored index gets. Namespaced by pack because it has to be: the
// same index is a different picture in a different pack, and a shared name
// space shows one map's art on another's walls the moment two packs are loaded.
//
//==========================================================================

// The masked variant of a picture is a separate texture under its own name: the
// hole is baked into the palette, and one picture serves many faces.
static FString PictureName(const std::string &pack, int index, bool masked)
{
	return FStringf("ROTH_%s_T%05d%s", pack.c_str(), index, masked ? "_M" : "");
}

static FString FrameName(const std::string &pack, int index, int frame, bool masked)
{
	return FStringf("ROTH_%s_T%05d%s_%02d", pack.c_str(), index, masked ? "_M" : "", frame);
}

static FString ColourName(const std::string &pack, int paletteIndex)
{
	return FStringf("ROTH_%s_C%03d", pack.c_str(), paletteIndex);
}

//==========================================================================
//
// Register one picture, with its animation frames if it has any.
//
// The texture manager is the authority on whether this already exists: it can
// be rebuilt underneath us, which would invalidate anything we had cached, so
// an existing name is always reused rather than re-registered.
//
//==========================================================================

FTextureID TextureSet::Build(int index, Log *log, bool masked)
{
	FString name = PictureName(mName, index, masked);
	FTextureID existing = TexMan.CheckForTexture(name.GetChars(), ETextureType::Override);
	if (existing.isValid()) return existing;

	// allFrames: run the delta decoder, so an animated image yields every frame.
	Image img = mPack->ReadImage(index, true);
	if (!img.ok())
	{
		mFailed++;
		if (log) log->Count("artwork: image could not be decoded");
		return FNullTextureID();
	}

	FRemapTable *remap = GPalette.GetTranslation(TRANSLATION_Standard,
		masked ? mTranslation : mOpaqueTranslation);

	// The quarter turn: exchange the dimensions and hand the stored bytes over
	// as column-major. See the file comment.
	const int w = img.height;
	const int h = img.width;

	auto addFrame = [&](const std::vector<uint8_t> &pixels, const char *texName) -> FTextureID
	{
		auto *image = new FPalettedMemoryImage(KeepPixels(pixels), remap, w, h, true, masked);
		auto *tex = MakeGameTexture(new FImageTexture(image), texName, ETextureType::Override);
		return TexMan.AddGameTexture(tex);
	};

	FTextureID base = addFrame(img.frames[0], name.GetChars());
	mRegistered++;

	// Animation is cheap here: the frames are registered immediately after the
	// base picture, so they occupy a contiguous run of texture ids, which is
	// exactly what a simple forward animation wants.
	const int frames = (int)img.frames.size();
	if (frames > 1 && base.isValid())
	{
		for (int f = 1; f < frames; f++)
		{
			addFrame(img.frames[f], FrameName(mName, index, f, masked).GetChars());
			mRegistered++;
		}
		TexAnim.AddSimpleAnim(base, frames, ANIM_FRAME_MS);
		mAnimated++;
		if (log) log->Count("artwork: animated images");
	}

	return base;
}

//==========================================================================
//
// A stored index at or above 32768 is not a picture but "fill this surface with
// one palette colour". Eight by eight rather than one by one so that nothing
// downstream has to cope with a degenerate size.
//
//==========================================================================

FTextureID TextureSet::SolidColour(int paletteIndex, Log *log)
{
	if (paletteIndex < 0 || paletteIndex > 255) paletteIndex = 0;

	FString name = ColourName(mName, paletteIndex);
	FTextureID existing = TexMan.CheckForTexture(name.GetChars(), ETextureType::Override);
	if (existing.isValid()) return existing;

	std::vector<uint8_t> pixels(8 * 8, (uint8_t)paletteIndex);
	FRemapTable *remap = GPalette.GetTranslation(TRANSLATION_Standard, mOpaqueTranslation);
	auto *image = new FPalettedMemoryImage(KeepPixels(pixels), remap, 8, 8, true, false);
	auto *tex = MakeGameTexture(new FImageTexture(image), name.GetChars(), ETextureType::Override);

	mSolidColours++;
	if (log) log->Count("artwork: solid-colour surfaces");
	return TexMan.AddGameTexture(tex);
}

//==========================================================================
//
//
//
//==========================================================================

bool TextureSet::IsNothing(int index) const
{
	return mPack && index == (int)mPack->SkyIndex();
}

FTextureID TextureSet::World(int index, Log *log, bool masked)
{
	if (!mPack || IsNothing(index)) return FNullTextureID();

	auto &memo = masked ? mByIndexMasked : mByIndex;
	auto found = memo.find(index);
	if (found != memo.end())
	{
		// Only trust the memo while the texture manager still agrees: it can be
		// rebuilt underneath us, which invalidates every id we handed out.
		if (TexMan.GetGameTexture(found->second, false) != nullptr)
			return found->second;
		memo.erase(found);
	}

	FTextureID id;
	// Nearest base first: 0xFF00 before 0x8000, or every high-cluster index
	// would be read against the wrong base and land outside the palette.
	if (index >= (int)TEX_COLOUR_BASE_HI)
		id = SolidColour(index - (int)TEX_COLOUR_BASE_HI, log);
	else if (index >= (int)TEX_COLOUR_BASE_LO)
		id = SolidColour(index - (int)TEX_COLOUR_BASE_LO, log);
	else if (index >= (int)TEX_PICTURE_LIMIT)
	{
		// Past where pictures stop, but matching neither colour base. No retail
		// map has an index here, so reaching this means either a modified map or
		// a wrong reading -- either way it is reported, not guessed at.
		mOutOfRange++;
		if (log) log->Count("artwork: index above the picture limit with no colour base");
		id = SolidColour(0, log);
	}
	else
		id = Build(index, log, masked);

	memo[index] = id;
	return id;
}

} // namespace roth
