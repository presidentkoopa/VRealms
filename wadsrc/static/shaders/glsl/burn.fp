/*
** burn.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2014-2016 Christoph Oelckers
** Copyright 2018-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=0) in vec4 vTexCoord;
layout(location=1) in vec4 vColor;
layout(location=0) out vec4 FragColor;
#ifdef SCENE_LIGHT_MASK
// [LIGHTMASK] The light mask (hw_postprocess.h, PPLightMask): no light of either class, with the
// colour's own alpha, so what this covers loses its share exactly as its colour is covered.
layout(location = LIGHT_MASK_LOCATION) out vec4 FragLightMask;
#endif
#ifdef SCENE_POST_MASK
// [SCENEMASK] The scene tag (main.fp's block says what this is): the draw's.
#define uPostMask data[uDataIndex].padding1
layout(location = POST_MASK_LOCATION) out vec4 FragPostMask;
#endif

void main()
{
	vec4 frag = vColor;

	vec4 t1 = texture(tex, vTexCoord.xy);
	vec4 t2 = texture(texture2, vec2(vTexCoord.x, 1.0-vTexCoord.y));

	FragColor = frag * vec4(t1.r, t1.g, t1.b, t2.a);
#ifdef SCENE_LIGHT_MASK
	FragLightMask = vec4(0.0, 0.0, 0.0, FragColor.a);
#endif
#ifdef SCENE_POST_MASK
	FragPostMask = vec4(float(uPostMask) * (1.0 / 255.0), 0.0, 0.0, 1.0);	// [SCENEMASK]
#endif
}
