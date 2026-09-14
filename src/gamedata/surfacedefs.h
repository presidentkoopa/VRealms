/*
** surfacedefs.h
**
** [SURFACEMATERIALS] What textures are made of: metal, wood, glass, liquid...
** Engine item #16 ("Engine docs/SURFACE_MATERIALS_PLAN.md"). The answer lives on
** the texture, FGameTexture::GetSurface() (NAME_None = untagged); script reads it
** with TexMan.GetSurface(tex). Footsteps, physics and weapon effects all ask the
** same question, so no mod keeps its own list of texture names.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** WHERE THE NAME COMES FROM, later wins
**
**   1. SURFACES lumps -- wildcard rules, every lump in load order, read by the
**      DEFBLOCKS reader (defblocks.h):
**
**        // * matches any run, ? one character. Patterns are quoted: the scanner
**        // splits * and ? off a bare word. A plain name may be a bare word.
**        surface metal
**        {
**            walls = "METAL*", "SUPPORT*", "DOOR*", EXITDOOR
**            flats = FLAT23, "STEP?"
**            any   = "*GRATE*"
**        }
**
**      walls covers Wall, WallPatch and Override textures (full-path textures are
**      Override); flats covers Flat; any covers every texture. It keys on the
**      texture's own type, not on what a trace hit. Case-insensitive. A later
**      rule beats an earlier one, so a map pack can reclassify; `surface none`
**      un-tags. A texture is matched by the name TexMan.GetName gives it.
**   2. GLDEFS `material texture "BRONZE1" { surface metal }` -- one texture,
**      exactly (TagTextureSurface). ParseGLDefs runs after the SURFACES loader.
**   3. TERRAIN `terrain Water modify { surface liquid }` -- every texture a
**      `floor` line maps to that terrain, unless 1 or 2 already decided it
**      (ApplyTerrainSurfaces). `defaultterrain` tags nothing.
**
** Textures created after load (a full-path name a map uses) are classified the
** moment the texture manager adds them (FTextureManager::GameTextureAddedHook).
**
** ERRORS NEVER ABORT. A bad block is refused with one line naming the lump, the
** line and the block; the rest loads.
**
** NETPLAY. Map/texture data, loaded the same way on every machine from the same
** lumps, with no RNG and no runtime setter: a texture's answer never changes
** after load. Case folding is ASCII-only, never the C locale's.
**
*/

#pragma once

#include "name.h"

class FGameTexture;

// Every SURFACES lump, then every texture. d_main.cpp, just before ParseGLDefs().
// Prints "Surfaces: <R> rules from <L> lumps, <T> textures tagged, <N> refused (<ms> ms)".
void LoadSurfaceDefinitions();

// An exact tag for one texture, beating any SURFACES rule. GLDEFS' `surface`
// keyword today; any other exact source tomorrow. `source` and `line` are kept
// for the `surface` console command.
void TagTextureSurface(FGameTexture *tex, FName surface, const char *source, int line);

// TERRAIN `surface`: every texture a `floor` line maps to such a terrain, when no
// rule or tag has decided it. p_terrain.cpp, at the end of P_InitTerrainTypes.
void ApplyTerrainSurfaces();
