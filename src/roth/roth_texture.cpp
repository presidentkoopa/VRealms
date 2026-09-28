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
// THE THREE WAYS A REALMS IMAGE MEETS THE FRAMEBUFFER.
//
// The original picks a span function per IMAGE, from bit 2 of the image's
// `image_type` byte -- the bit roth_das.h calls IT_TRANSLUCENT. Two independent
// sites set it, and they agree:
//
//   renderer.c:6288   g_span_textured_mode_flag = byte[block + 0xb] & 4;
//   renderer.c:13278  eax = dword[block + 8];   // bits 16..31 are the flags word
//                     if (eax & 0x04000000) mode = 0xff; else mode = 0;
//
// (block + 0xa is the flags word, whose high byte is image_type, so 0x04000000
// and `byte[block+0xb] & 4` are the same bit.)
//
// When the bit is SET the span function is renderer.c:2635 / 11727, whose
// per-texel rule is exactly:
//
//   if (texel == 0)          skip           -- a hole
//   else if (texel & 0x80)   dst = blend[(dst << 8) | colour]   -- see through
//   else                     dst = colour   -- opaque
//
// When it is CLEAR every texel is written, index 0 included.
//
// VERIFIED, not assumed: `blend` is the 64K table the DAS file stores right
// after its palette (map_load.c:377 reads 0x10000 bytes into the buffer whose
// selector is g_transparency_blend_selector). Reading DEMO.DAS's copy of it and
// fitting an alpha over every (dst, src) pair gives a clean minimum at 0.50,
// and blend[(x << 8) | x] == x exactly for every x. It is a 50% average.
//
// All of that maps onto a palette's alpha channel, which is what
// FPalettedMemoryImage already consumes, so it needs no renderer work: alpha 0
// is the hole CopyPixelData skips, alpha 128 is the 50% blend, alpha 255 is
// opaque. Hence three tables rather than two.
//
//==========================================================================

// The 0x80 blend is a 50% average of the source and the destination -- measured
// off the retail blend table, see above.
static const uint8_t BLEND_ALPHA = 128;

// Indices from here up are the translucent half of the palette in an image the
// original draws with the transparency+translucency span function.
static const int BLEND_INDEX_FIRST = 0x80;

struct PackTranslations
{
	int opaque = -1;      // every index written, index 0 included
	int keyed = -1;       // index 0 is a hole, everything else opaque
	int translucent = -1; // index 0 is a hole, index >= 0x80 is a 50% blend
};

// Cached per pack, because a pack's palette does not change between loads and
// the translation manager has no way to retire one. Without this, loading the
// same map twice would store a second identical set.
static std::map<std::string, PackTranslations> TranslationsByPack;

static int MakeTranslation(const std::vector<Colour> &palette, bool zeroTransparent,
	bool blendHighIndices)
{
	FRemapTable opal;
	for (int c = 0; c < 256; c++)
	{
		const Colour &col = palette[c];
		// The paletted renderer cannot express a per-index blend, so Remap[]
		// stays the nearest opaque match either way; the alpha in Palette[] is
		// what the true-colour path reads.
		const uint8_t alpha = (blendHighIndices && c >= BLEND_INDEX_FIRST)
			? BLEND_ALPHA : 255;
		opal.Palette[c] = PalEntry(alpha, col.r, col.g, col.b);
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
		mTranslation = cached->second.keyed;
		mOpaqueTranslation = cached->second.opaque;
		mTranslucentTranslation = cached->second.translucent;
	}
	else
	{
		PackTranslations t;
		t.keyed = MakeTranslation(mPack->Palette(), true, false);
		t.opaque = MakeTranslation(mPack->Palette(), false, false);
		t.translucent = MakeTranslation(mPack->Palette(), true, true);
		TranslationsByPack[mName] = t;
		mTranslation = t.keyed;
		mOpaqueTranslation = t.opaque;
		mTranslucentTranslation = t.translucent;
	}

	if (log)
	{
		log->Line("  pack %-8s entries %5d   header word +0x22 %5d",
			mName.c_str(), mPack->Count(), (int)mPack->SkyMarkerIndex());
	}
	return true;
}

//==========================================================================
//
// The map's sky picture.
//
// THE SKY IS NAMED BY THE MAP, NOT BY THE PACK, and it is an ordinary picture:
// the metadata word at +0x18 (roth_raw.h `Metadata::skyTexture`) becomes
// `g_das_special_fat_index` (map_load.c:214, renderer.c:10306), which
// render_parallax_sky_columns resolves a normal DAS block from
// (renderer.c:5414). Nothing in the original makes an index mean "draw
// nothing"; that reading came from the pack header word at +0x22, which is
// something else entirely (see roth_das.h).
//
// Opaque and unmasked, because the parallax column renderer writes every texel
// it samples -- a sky with holes in it would show the void through the window.
//
// This is a named door onto World() rather than new machinery, so that the
// place a caller reaches for the sky carries the explanation with it.
//
//==========================================================================

bool TextureSet::IsSkySurface(int index) const
{
	// The marker is a real, painted entry in every pack (DEMO's 0 is a fully
	// opaque 256 x 146 picture), so this cannot be decided by looking at the
	// artwork -- only the pack header says which index is the sky.
	return mPack != nullptr && index >= 0
		&& (uint16_t)index == mPack->SkyMarkerIndex();
}

bool TextureSet::ImageShape(int index, int &w, int &h, bool &translucent) const
{
	if (mPack == nullptr || index < 0) return false;
	// Header only: allFrames would run the animation delta decoder, and all this
	// answers is the shape and blend of the still image.
	const Image img = mPack->ReadImage(index, false);
	if (!img.ok()) return false;
	w = img.width;
	h = img.height;
	translucent = (img.imageType & IT_TRANSLUCENT) != 0;
	return true;
}

FTextureID TextureSet::Sky(int metadataSkyIndex, Log *log)
{
	return World(metadataSkyIndex, log, false);
}

//==========================================================================
//
// The name a stored index gets. Namespaced by pack because it has to be: the
// same index is a different picture in a different pack, and a shared name
// space shows one map's art on another's walls the moment two packs are loaded.
//
//==========================================================================

// How a picture's pixels reach the framebuffer, which is part of its identity:
// the hole and the blend are baked into the palette, and one picture serves
// many faces, so each way of drawing it is a texture of its own.
enum class Blend
{
	Opaque,      // ""   every index written
	Keyed,       // "_M" index 0 is a hole
	Translucent, // "_T" index 0 is a hole, index >= 0x80 is a 50% blend
};

static const char *BlendSuffix(Blend b)
{
	return b == Blend::Keyed ? "_M" : b == Blend::Translucent ? "_T" : "";
}

static FString PictureName(const std::string &pack, int index, Blend b)
{
	return FStringf("ROTH_%s_T%05d%s", pack.c_str(), index, BlendSuffix(b));
}

static FString FrameName(const std::string &pack, int index, int frame, Blend b)
{
	return FStringf("ROTH_%s_T%05d%s_%02d", pack.c_str(), index, BlendSuffix(b), frame);
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

FTextureID TextureSet::Build(int index, Log *log, bool masked, bool flipped)
{
	// allFrames: run the delta decoder, so an animated image yields every frame.
	Image img = mPack->ReadImage(index, true);
	if (!img.ok())
	{
		mFailed++;
		if (log) log->Count("artwork: image could not be decoded");
		return FNullTextureID();
	}

	//----------------------------------------------------------------------
	// HOW THIS PICTURE IS DRAWN IS THE PICTURE'S OWN PROPERTY, not the
	// surface's. See the Blend comment above: the original reads bit 2 of
	// image_type and picks a span function, and nothing about the face reaches
	// that decision.
	//
	// The caller's `masked` hint is kept as an ADDITION rather than replaced,
	// because taking it away would make every two-sided mid piece opaque at
	// once and that is a change nobody has looked at yet. Strictly, ROTH.C
	// keys masking off the image alone and the hint should go; it is a
	// superset of the right answer today, so it can only leave a hole where
	// the original drew a colour, never the reverse.
	//----------------------------------------------------------------------
	const bool imageIsTranslucent = (img.imageType & IT_TRANSLUCENT) != 0;
	const Blend blend = imageIsTranslucent ? Blend::Translucent
		: masked ? Blend::Keyed : Blend::Opaque;

	FString name = PictureName(mName, index, blend);
	if (flipped) name += "_X";
	FTextureID existing = TexMan.CheckForTexture(name.GetChars(), ETextureType::Override);
	if (existing.isValid()) return existing;

	FRemapTable *remap = GPalette.GetTranslation(TRANSLATION_Standard,
		blend == Blend::Translucent ? mTranslucentTranslation
		: blend == Blend::Keyed ? mTranslation : mOpaqueTranslation);

	if (imageIsTranslucent)
	{
		mTranslucent++;
		if (log) log->Count("artwork: translucent images (IT_TRANSLUCENT)");
	}

	// The quarter turn: exchange the dimensions and hand the stored bytes over
	// as column-major. See the file comment.
	const int w = img.height;
	const int h = img.width;

	// bMasked: true wherever a texel can be a hole or a blend, which is what
	// makes the image source report itself as possibly translucent.
	const bool hasHoles = (blend != Blend::Opaque);

	// MIRRORED VARIANT. The original flips by computing u' = limit - u - 1
	// (renderer.c:4734, `ax = ~ax + [0x90986]`) -- a mirror INSIDE the texture's
	// own texel range, not across the wall piece. Doom's negative scale mirrors
	// the whole piece instead, so on any wall carrying more than one copy the
	// tiles land in different places and the seams do not meet. Mirroring the
	// image itself is what the original actually does.
	//
	// The buffer is column-major with `w` columns of `h`, so a horizontal mirror
	// is a reversal of the column order.
	auto mirrorColumns = [&](const std::vector<uint8_t> &src) -> std::vector<uint8_t>
	{
		std::vector<uint8_t> out(src.size());
		const size_t col = (size_t)h;
		for (int x = 0; x < w; x++)
		{
			const size_t from = (size_t)x * col;
			const size_t to   = (size_t)(w - 1 - x) * col;
			if (from + col <= src.size() && to + col <= out.size())
				memcpy(&out[to], &src[from], col);
		}
		return out;
	};

	auto addFrame = [&](const std::vector<uint8_t> &pixelsIn, const char *texName) -> FTextureID
	{
		const std::vector<uint8_t> pixels = flipped ? mirrorColumns(pixelsIn) : pixelsIn;
		auto *image = new FPalettedMemoryImage(KeepPixels(pixels), remap, w, h, true, hasHoles);
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
			FString fn = FrameName(mName, index, f, blend);
			if (flipped) fn += "_X";
			addFrame(img.frames[f], fn.GetChars());
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

// NO INDEX MEANS "DRAW NOTHING". There used to be an IsNothing() here that
// suppressed the pack header's +0x22 word, on the reading that it named an
// index Realms used as a blank. It does not: it is not an index into anything
// this class registers (roth_das.h has the evidence), and suppressing it threw
// away a real picture -- in DEMO that was entry 0, a fully painted 256 x 146
// image, which is why the sectors asking for it rendered black.
FTextureID TextureSet::World(int index, Log *log, bool masked, bool flipped)
{
	if (!mPack) return FNullTextureID();

	// Four variants now: masked or not, mirrored or not. A mirrored image is a
	// separate registration because the mirror lives in the pixels, not in a
	// draw-time scale.
	auto &memo = flipped ? (masked ? mByIndexMaskedFlipped : mByIndexFlipped)
	                     : (masked ? mByIndexMasked : mByIndex);
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
		id = Build(index, log, masked, flipped);

	memo[index] = id;
	return id;
}

} // namespace roth
