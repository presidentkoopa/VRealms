/*
** fogboundary.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2019-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=2) in vec4 pixelpos;
layout(location=0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location=1) out vec4 FragFog;
layout(location=2) out vec4 FragNormal;
#endif
#ifdef SCENE_LIGHT_MASK
// [LIGHTMASK] The light mask (hw_postprocess.h, PPLightMask): fog is light of neither class. Written
// with the colour's alpha, so the fog in front of a beam takes the beam's share down with its colour.
layout(location = LIGHT_MASK_LOCATION) out vec4 FragLightMask;
#endif

//===========================================================================
//
// Main shader routine
//
//===========================================================================

void main()
{
	float fogdist;
	float fogfactor;

	//
	// calculate fog factor
	//
	if (uFogEnabled == -1)
	{
		fogdist = pixelpos.w;
	}
	else
	{
		fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
	}
	fogfactor = exp2 (uFogDensity * fogdist);
	FragColor = vec4(uFogColor.rgb, 1.0 - fogfactor);
#ifdef SCENE_LIGHT_MASK
	FragLightMask = vec4(0.0, 0.0, 0.0, FragColor.a);
#endif
#ifdef GBUFFER_PASS
	FragFog = vec4(0.0, 0.0, 0.0, 1.0);
	FragNormal = vec4(0.5, 0.5, 0.5, 1.0);
#endif
}
