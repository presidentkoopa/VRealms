/*
** damagedefs.h
**
** [SURFACEDAMAGE] The surface damage brushes and looks: the DAMAGEDEFS lumps, read by the shared definitions reader.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SURFACE_DAMAGE_PLAN.md" #17 (owner answer 13: brushes go through the defblocks reader like every effect
** kind, over as many lumps as a mod likes) and "Engine docs/WALL_DAMAGE_ART_PLAN.md" (the ballistics lane's brushes, asks 1
** and 2). Every lump whose full name is DAMAGEDEFS (DAMAGEDEFS, DAMAGEDEFS.txt, ...) in load order; a later block of the
** same kind and name replaces an earlier one.
**
**   // A brush: what one PaintSurfaceDamage presses into a surface. Its masks are images in the engine's channels -- red
**   // soot, green hole depth, blue heat, alpha wetness -- each multiplied by the paint's own soot, depth, heat and wet.
**   brush hole_chip
**   {
**       masks = "damage/hole_chip_a.png", "damage/hole_chip_b.png"   // 1..16 variants, picked by a hash of the position
**       channels = rgba          // rgba (default); alpha: the image's alpha is all four masks; luminance: its brightness
**       turn = none              // none (default): unrotated unless the paint gives an axis; hashed: turned by the position hash
**       flip = none              // none (default); hashed: half the paints mirrored, by the position hash
**   }
**
**   // A look: how damage shows on one kind of surface -- the #16 surface name (SURFACES, GLDEFS `surface`, TERRAIN), or
**   // `default` for every surface without a look of its own.
**   look metal
**   {
**       rim = 1.0 0.97 0.92      // the colour a hole's edge takes
**       rimstrength = 0.8        // 0..1
**       rimwidth = 6             // how far out from a hole's edge the rim reaches (bigger = wider)
**       inside = 0.1 0.1 0.11    // what the inside of a hole multiplies the surface by
**       insidestrength = 0.9     // 0..1
**       detail = "damage/metal_grain.png"   // a tiling image that breaks holes and rims up; left off: the engine's grain
**       detailscale = 24         // map units per repeat
**       detailstrength = 0.5     // 0..1
**       wetshine = 1.0           // how much wetness raises specular (0..4)
**       bend = 1.0               // how much holes bend the surface for dynamic lights (0..4)
**       depth = 1.5              // how far (map units) the texture inside a hole is pushed in when looked at (0 off; 0..8)
**   }
**
** Built in, before any lump: the brush `round` (a soft bowl: depth, soot, heat and wet all fall off from its centre) and
** the looks default (stone and concrete: a dusty grey rim), metal (bright dent edges), wood (splintered pale edges), dirt,
** glass and liquid. A lump's `look metal` replaces the built-in one; keys it leaves out keep the built-in values of that
** name, or of `default` for a new name.
**
** ERRORS NEVER ABORT: a refused block prints one line with its lump and line and adds nothing; the rest loads.
**
** NETPLAY: the images are decoded straight from their lumps (FImageSource), never registered as textures, so loading them
** changes no texture number on any machine. A brush's variant salt is a hash of its name's text, the same everywhere.
**
*/

#pragma once

#include <cstdint>
#include "tarray.h"
#include "zstring.h"
#include "name.h"

struct SurfaceDamageBrush
{
	FName Name = NAME_None;
	int FirstLayer = 0;			// the first variant's layer in the brush atlas
	int Variants = 1;
	bool TurnHashed = false;
	bool FlipHashed = false;
	uint32_t Salt = 0;			// the name's text hashed, lower case: the variant pick's salt, the same on every machine
	FString Lump;				// "" = built in
	int Line = 0;
};

struct SurfaceDamageLook
{
	FName Surface = NAME_None;	// NAME_None = `default`
	float Rim[4] = {};			// rgb, strength
	float Inside[4] = {};		// rgb, strength
	float Detail[4] = {};		// detail layer, map units per repeat, strength, rim width
	float Finish[4] = {};		// wet shine, 0, bend, parallax depth
	FString DetailTexture;		// a lump's full name; "" = the engine's grain (detail layer 0)
	FString Lump;				// "" = built in
	int Line = 0;
};

struct SurfaceDamageDefinitionSet
{
	TArray<SurfaceDamageBrush> Brushes;	// [0] is the built-in `round`
	TArray<SurfaceDamageLook> Looks;	// [0] is `default`
	// Whole mip chains, level 0 of every layer, then level 1 of every layer, ... (hw_surfacedamageframe.h), RGBA8 in the
	// engine's channels.
	TArray<uint8_t> BrushPixels;
	int BrushLayers = 0;
	TArray<uint8_t> DetailPixels;
	int DetailLayers = 0;
	uint64_t Generation = 0;			// rises with every load
	unsigned Lumps = 0;
	int Blocks = 0;
	int Refused = 0;
	double LoadMs = 0.0;
};

// Reads every DAMAGEDEFS lump and decodes the images. Called by the renderer's surface damage the first time it is needed
// (SurfaceDamage::PrepareFrame); calling it again reloads everything.
void LoadSurfaceDamageDefinitions();
const SurfaceDamageDefinitionSet& SurfaceDamageDefinitions();

// A brush by name, or -1. A look for a #16 surface name: its index, or 0 (`default`) when it has none.
int SurfaceDamageFindBrush(FName name);
int SurfaceDamageLookFor(FName surface);
