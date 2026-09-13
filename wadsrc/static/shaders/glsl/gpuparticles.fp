/*
** gpuparticles.fp
**
** [GPUPARTICLES] Soft round emissive dot. Colour, intensity, fade over life and
** the intensity cvar are already folded into vParticleColor by
** gpuparticles.vp; this only shapes it.
**
** Drawn with STYLE_Add, whose source factor is source alpha, so the round
** falloff goes in alpha and the blend applies it. Additive blending does not
** depend on draw order, which is why nothing is sorted. No lighting, no
** texture in phase one.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

layout(location = 0) in vec2 vParticleCorner;
layout(location = 1) in vec4 vParticleColor;

layout(location = 0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location = 1) out vec4 FragFog;
layout(location = 2) out vec4 FragNormal;
#endif

void main()
{
	float r2 = dot(vParticleCorner, vParticleCorner);
	float falloff = clamp(1.0 - r2, 0.0, 1.0);
	falloff *= falloff;

#ifdef SCENE_DEPTH_READ
	// [2a] SOFT EDGE. Only in the scene-depth variants (vk_shader.cpp,
	// sceneDepthBindings), which are only drawn inside the read-only depth pass
	// RenderTranslucent opens while r_gpuparticles_soft is above 0. The ordinary
	// gpuparticles program never sees this block.
	//
	// Fade to nothing over uGpuParticleParams2.x map units in front of whatever
	// surface is behind this pixel, so a spark meeting a wall fades into it rather
	// than cutting off along the intersection. Both distances go through the same
	// linearization, so the fade reaches exactly 0 at the surface.
	float softDistance = uGpuParticleParams2.x;
	if (softDistance > 0.0)
	{
		float sceneDistance = SceneDepthLinear(SceneDepthRaw());
		float ownDistance = SceneDepthLinear(gl_FragCoord.z);
		falloff *= clamp((sceneDistance - ownDistance) / softDistance, 0.0, 1.0);
	}
#endif

	FragColor = vec4(vParticleColor.rgb, falloff);

#ifdef GBUFFER_PASS
	// Zero with zero alpha: under additive blending this adds nothing to the
	// fog and normal attachments.
	FragFog = vec4(0.0);
	FragNormal = vec4(0.0);
#endif
}
