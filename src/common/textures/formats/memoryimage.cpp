/*
** memoryimage.cpp
**
** An 8-bit palette-indexed image built in memory rather than read from a lump.
**
** See the class comment on FPalettedMemoryImage in image.h for the contract:
** the caller owns the pixels, the caller owns the palette, and the caller says
** which way round the rows and columns run.
**
** Written for loaders that decode a foreign game's artwork off the player's own
** disk at load time. It deliberately knows nothing about any particular format.
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include "bitmap.h"
#include "image.h"
#include "palettecontainer.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "printf.h"

#include <chrono>

//==========================================================================
//
// Conversion accounting. See the comment on ConversionStats in image.h.
//
// Not thread-safe by design: a background texture loader would have to make
// these atomic, and paying for that on every conversion to serve a diagnostic
// would be the wrong trade. A lost increment does not change the answer these
// numbers exist to give.
//
//==========================================================================

// Off by default: this is a diagnostic, and the interesting question is WHEN
// the conversions happen, which needs them interleaved with the rest of the
// console log. Set it before loading the level to capture the whole burst.
CVAR(Bool, img_conversion_log, false, 0)

static FPalettedMemoryImage::ConversionStats TrueColour, Paletted;

const FPalettedMemoryImage::ConversionStats& FPalettedMemoryImage::TrueColourStats() { return TrueColour; }
const FPalettedMemoryImage::ConversionStats& FPalettedMemoryImage::PalettedStats() { return Paletted; }

void FPalettedMemoryImage::ResetStats()
{
	TrueColour = ConversionStats();
	Paletted = ConversionStats();
}

namespace
{

class ScopedConversionTimer
{
public:
	ScopedConversionTimer(FPalettedMemoryImage::ConversionStats& into, uint64_t pixels)
		: mInto(into), mPixels(pixels), mStart(std::chrono::steady_clock::now()) {}

	~ScopedConversionTimer()
	{
		const double ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - mStart).count();
		mInto.count++;
		mInto.pixels += mPixels;
		mInto.totalMs += ms;
		if (ms > mInto.worstMs) mInto.worstMs = ms;

		// A running trace, so the burst can be located in time rather than
		// only counted. One line per 64 conversions plus the first.
		if (img_conversion_log && (mInto.count == 1 || (mInto.count % 64) == 0))
		{
			Printf("imagestats: %llu conversions, %.2f ms total, %.2f ms worst\n",
				(unsigned long long)mInto.count, mInto.totalMs, mInto.worstMs);
		}
	}

private:
	FPalettedMemoryImage::ConversionStats& mInto;
	uint64_t mPixels;
	std::chrono::steady_clock::time_point mStart;
};

} // namespace

CCMD(imagestats)
{
	auto print = [](const char* what, const FPalettedMemoryImage::ConversionStats& s)
	{
		Printf("  %-12s %6llu conversions  %10llu px  %8.2f ms total  %7.2f ms worst\n",
			what, (unsigned long long)s.count, (unsigned long long)s.pixels,
			s.totalMs, s.worstMs);
	};
	Printf("In-memory paletted image conversions since startup:\n");
	print("true colour", FPalettedMemoryImage::TrueColourStats());
	print("paletted", FPalettedMemoryImage::PalettedStats());
}

//==========================================================================
//
//
//
//==========================================================================

FPalettedMemoryImage::FPalettedMemoryImage(const uint8_t* pixels, FRemapTable* translation,
	int width, int height, bool columnMajor, bool masked)
	: RawPixels(pixels), Translation(translation), ColumnMajor(columnMajor)
{
	Width = width;
	Height = height;
	// Not a sprite: nothing here has an offset of its own. A caller that needs
	// one sets it through the game texture.
	LeftOffset = 0;
	TopOffset = 0;
	bMasked = masked;
	// "Might have holes" is the only safe default for an image with a hole, and
	// a solid one says so. Translucency still needs a real check either way.
	bTranslucent = masked ? -1 : 0;
}

//==========================================================================
//
// The paletted renderer's copy. GZDoom's own order is column-major, so a
// column-major source is a straight remap and a row-major one transposes.
//
//==========================================================================

PalettedPixels FPalettedMemoryImage::CreatePalettedPixels(int conversion, int frame)
{
	ScopedConversionTimer timer(Paletted, (uint64_t)Width * Height);

	PalettedPixels Pixels(Width * Height);
	FRemapTable* Remap = Translation;
	const bool luminous = (conversion == luminance);

	if (ColumnMajor)
	{
		const int count = Width * Height;
		for (int i = 0; i < count; i++)
		{
			auto c = RawPixels[i];
			Pixels[i] = luminous ? Remap->Palette[c].Luminance() : Remap->Remap[c];
		}
	}
	else
	{
		for (int x = 0; x < Width; x++)
		{
			for (int y = 0; y < Height; y++)
			{
				auto c = RawPixels[y * Width + x];
				Pixels[x * Height + y] = luminous ? Remap->Palette[c].Luminance() : Remap->Remap[c];
			}
		}
	}
	return Pixels;
}

//==========================================================================
//
// The true-colour copy, which uses the real palette and so keeps the source
// artwork's exact colours. CopyPixelData reads patch[y*step_y + x*step_x] for
// destination pixel (x, y), which is what picks the source order out.
//
//==========================================================================

int FPalettedMemoryImage::CopyPixels(FBitmap* bmp, int conversion, int frame)
{
	ScopedConversionTimer timer(TrueColour, (uint64_t)Width * Height);

	PalEntry* Remap = Translation->Palette;
	if (ColumnMajor)
		bmp->CopyPixelData(0, 0, RawPixels, Width, Height, Height, 1, 0, Remap);
	else
		bmp->CopyPixelData(0, 0, RawPixels, Width, Height, 1, Width, 0, Remap);
	return -1;
}
