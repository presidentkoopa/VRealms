/*
** stencil.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2014-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location=1) out vec4 FragFog;
layout(location=2) out vec4 FragNormal;
#endif
#ifdef SCENE_LIGHT_MASK
// [LIGHTMASK] The light mask (hw_postprocess.h, PPLightMask): nothing, with the colour's alpha 0.
layout(location = LIGHT_MASK_LOCATION) out vec4 FragLightMask;
#endif
#ifdef SCENE_POST_MASK
// [SCENEMASK] The scene tag (main.fp's block says what this is). Declared and written because
// Vulkan leaves an attachment a fragment shader does not write UNDEFINED -- but a portal stencil
// draws with the colour mask off, and the pipeline then masks this attachment off too
// (VkRenderPassSetup::CreatePipeline), so what a stencil covers keeps the tag it had.
#define uPostMask data[uDataIndex].padding1
layout(location = POST_MASK_LOCATION) out vec4 FragPostMask;
#endif

void main()
{
	FragColor = vec4(1.0, 1.0, 1.0, 0.0);
#ifdef GBUFFER_PASS
	FragFog = vec4(0.0, 0.0, 0.0, 1.0);
	FragNormal = vec4(0.5, 0.5, 0.5, 1.0);
#endif
#ifdef SCENE_LIGHT_MASK
	FragLightMask = vec4(0.0);
#endif
#ifdef SCENE_POST_MASK
	FragPostMask = vec4(float(uPostMask) * (1.0 / 255.0), 0.0, 0.0, 1.0);	// [SCENEMASK]
#endif
}
