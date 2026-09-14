/*
** gpuparticles.fp
**
** [GPUPARTICLES] Soft round emissive dot -- or, [2c], a flipbook frame from the
** particle atlas. Colour, intensity, fade over life and the intensity cvar are
** already folded into vParticleColor by gpuparticles.vp; this only shapes it.
**
** [2d] TWO OUTPUTS, one per blend RenderTranslucent can draw the ring with; it
** writes which into uGpuParticleParams2.y, so the pipeline and this always agree:
**   0  STYLE_Add, whose source factor is source alpha, so the shape goes in alpha
**      and the blend applies it -- exactly the output before 2d. Used whenever no
**      occluding particle is alive, which is every additive effect on its own.
**   1  PREMULTIPLIED alpha (One, InvSrcAlpha), while a particle whose definition
**      has alpha above 0 is alive: rgb = lit body x occlusion + emissive, alpha =
**      occlusion. A purely additive particle has occlusion 0, so it adds exactly
**      the colour output 0 adds.
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
// [2c] x the atlas layer of the frame showing (-1 = untextured)  y the next frame's
// layer  z how far into it, 0..1  w the definition slot (-1 legacy). One value per
// particle, from gpuparticles.vp's ParticleFlipbookFrames.
layout(location = 2) flat in vec4 vParticleFlipbook;
// [2d] rgb the lit body colour  a the occlusion 0..1 (gpuparticles.vp).
layout(location = 3) flat in vec4 vParticleBody;
// [2d] The definition's soft distance: -1 = not set (r_gpuparticles_soft), 0 = hard.
layout(location = 4) flat in float vParticleSoft;

// [2c] THE PARTICLE ATLAS, fixed set binding 4 (VkTextureManager::ParticleAtlas):
// every textured definition's frames, one layer each, PREMULTIPLIED alpha, with
// mips. Declared here and not in vk_shader.cpp's shared prolog: only this effect
// reads it, so no other shader is touched. Vulkan only, like the whole effect.
layout(set = 0, binding = 4) uniform sampler2DArray ParticleAtlas;

layout(location = 0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location = 1) out vec4 FragFog;
layout(location = 2) out vec4 FragNormal;
#endif

void main()
{
	// [2c] The quad's corner as atlas coordinates: row 0 is the top of the art, and
	// the corner's y points up. The corner is not turned by spin, so the art turns
	// with the quad. Derivatives are taken here, outside any branch, and passed to
	// textureGrad, so the mip level never depends on which branch a pixel takes.
	vec2 atlasCoord = vec2(0.5 + 0.5 * vParticleCorner.x, 0.5 - 0.5 * vParticleCorner.y);
	vec2 atlasCoordDx = dFdx(atlasCoord);
	vec2 atlasCoordDy = dFdy(atlasCoord);

	// falloff    how much of the EMISSIVE colour shows here (the output before 2d)
	// bodyShade  [2d] what the body colour is multiplied by here
	// coverage   [2d] how much of the body is here
	float falloff;
	vec3 bodyShade;
	float coverage;
	if (vParticleFlipbook.x < -0.5)
	{
		// No texture: the soft round dot, exactly as before 2c.
		float r2 = dot(vParticleCorner, vParticleCorner);
		falloff = clamp(1.0 - r2, 0.0, 1.0);
		falloff *= falloff;
		bodyShade = vec3(falloff);
		coverage = falloff;
	}
	else
	{
		// [2c] FLIPBOOK. This frame and the next, mixed by how far the particle is into
		// the next, so 8 fps art stays smooth at 90 Hz. The atlas is premultiplied, so
		// neither this mix nor the filtering bleeds transparent pixels' colour.
		vec4 frameNow = textureGrad(ParticleAtlas, vec3(atlasCoord, vParticleFlipbook.x), atlasCoordDx, atlasCoordDy);
		vec4 frameNext = textureGrad(ParticleAtlas, vec3(atlasCoord, vParticleFlipbook.y), atlasCoordDx, atlasCoordDy);
		vec4 atlasTexel = mix(frameNow, frameNext, vParticleFlipbook.z);

		// The emissive part is scaled by the texel's luminance ("Engine docs/
		// GPU_PARTICLES_STAGE2_PLAN.md" 2c); premultiplied, that luminance already
		// carries the texture's alpha as coverage. [2d] The body takes the texel's own
		// colour -- premultiplied rgb is colour x texel alpha, the body's colour and
		// coverage at once -- and its alpha multiplies the alpha ramp.
		falloff = dot(atlasTexel.rgb, vec3(0.2126, 0.7152, 0.0722));
		bodyShade = atlasTexel.rgb;
		coverage = atlasTexel.a;
	}

#ifdef SCENE_DEPTH_READ
	// [2a] SOFT EDGE. Only in the scene-depth variants (vk_shader.cpp,
	// sceneDepthBindings), which are only drawn inside the read-only depth pass
	// RenderTranslucent opens while soft particles are wanted. The ordinary
	// gpuparticles program never sees this block.
	//
	// Fade to nothing over the soft distance in front of whatever surface is behind
	// this pixel, so a spark meeting a wall fades into it rather than cutting off
	// along the intersection. Both distances go through the same linearization, so
	// the fade reaches exactly 0 at the surface. [2d] The distance is the
	// definition's own when it sets one (0 = hard), else r_gpuparticles_soft; glow and
	// body fade together.
	float softDistance = vParticleSoft >= 0.0 ? vParticleSoft : uGpuParticleParams2.x;
	if (softDistance > 0.0)
	{
		float sceneDistance = SceneDepthLinear(SceneDepthRaw());
		float ownDistance = SceneDepthLinear(gl_FragCoord.z);
		float softFade = clamp((sceneDistance - ownDistance) / softDistance, 0.0, 1.0);
		falloff *= softFade;
		bodyShade *= softFade;
		coverage *= softFade;
	}
#endif

	if (uGpuParticleParams2.y > 0.5)
	{
		// [2d] PREMULTIPLIED: the lit body over what is behind it, the glow added on
		// top. With no body (occlusion 0) this is the additive output's colour.
		float occlusion = vParticleBody.a * coverage;
		FragColor = vec4(vParticleBody.rgb * (vParticleBody.a * bodyShade) + vParticleColor.rgb * falloff, occlusion);
	}
	else
	{
		FragColor = vec4(vParticleColor.rgb, falloff);
	}

#ifdef GBUFFER_PASS
	// Zero with zero alpha: under additive blending this adds nothing to the fog
	// and normal attachments, and [2d] under premultiplied blending (dst x (1 - 0))
	// it leaves them as they are too.
	FragFog = vec4(0.0);
	FragNormal = vec4(0.0);
#endif
}
