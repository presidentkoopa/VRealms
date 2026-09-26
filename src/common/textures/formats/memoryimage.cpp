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
	PalEntry* Remap = Translation->Palette;
	if (ColumnMajor)
		bmp->CopyPixelData(0, 0, RawPixels, Width, Height, Height, 1, 0, Remap);
	else
		bmp->CopyPixelData(0, 0, RawPixels, Width, Height, 1, Width, 0, Remap);
	return -1;
}
