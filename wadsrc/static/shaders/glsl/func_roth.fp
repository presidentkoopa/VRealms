/*
** func_roth.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2020-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

// Realms of the Haunting material. Identical to the default material; what
// makes it different is the ROTH_PALETTE define this shader is registered
// with (roth_palshade.cpp), which turns on the palette shading in main.fp, and
// its two custom textures: rothinvlut (colour -> palette index) and rothcmap
// (shade row x palette index -> colour, the DAS's own 32-level tables).
void SetupMaterial(inout Material material)
{
	SetMaterialProps(material, vTexCoord.st);
}
