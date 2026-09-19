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
** [LOOKS] GENERATED LOOKS ("Engine docs/GPU_PARTICLE_LOOKS_PLAN.md", build A). A particle
** whose definition has a look -- vParticleLife.w above 0, written by gpuparticles.vp -- is
** shaped by ParticleLook instead of the dot or the flipbook: fractal value noise in the
** particle's own space, world-sized and moved by the particle's own age, so both eyes and
** every machine see the same shape. It returns the same three numbers both blends already
** use, plus the colour the glow is multiplied by: the fixed heat ramp for fire, 1 otherwise.
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
// [LOOKS] x the life fraction 0..1  y the age in seconds  z the seed 0..1  w the drawn diameter
// in map units -- or all zero: no look (gpuparticles.vp).
layout(location = 5) flat in vec4 vParticleLife;
// [LOOKS] xyz the direction the view lights come from, in the quad's own axes (x, y those of
// vParticleCorner, z toward the eye)  w how much of the light comes from it, x lit. Zero unless
// a lit dust look is drawn at quality 2 or more with a light reaching it.
layout(location = 6) flat in vec4 vParticleLightDir;

// [2c] THE PARTICLE ATLAS, fixed set binding 4 (VkTextureManager::ParticleAtlas):
// every textured definition's frames, one layer each, PREMULTIPLIED alpha, with
// mips. Declared here and not in vk_shader.cpp's shared prolog: only this effect
// reads it, so no other shader is touched. Vulkan only, like the whole effect.
layout(set = 0, binding = 4) uniform sampler2DArray ParticleAtlas;
// [ATLASBC7] THE COMPRESSED PARTICLE ATLAS, fixed set binding 10 (VkTextureManager::ParticleAtlasCompressed): flipbooks stored as
// premultiplied BC7 DDS frames, one layer each, uploaded as stored with their own mips. Read with ParticleAtlas's sampler,
// coordinates and derivatives, for a definition whose look.w has PDF_ATLAS_COMPRESSED (2) ("Engine docs/
// PARTICLE_ATLAS_COMPRESSED_IMPL_NOTES.md").
layout(set = 0, binding = 10) uniform sampler2DArray ParticleAtlasCompressed;

// [LOOKS] hash13 / valueNoise, the noise drawnlines.fp's turbulence already uses.
#include "shaders/glsl/valuenoise.glsl"

// [LOOKS] A definition's look, in the record's spare vec4s (particledefs.h, EParticleLook),
// read from the definition buffer (set 1 binding 7, vertex and fragment) by the slot
// vParticleFlipbook.w carries -- only inside the look branch.
//   ParticleLookShape  x kind (0 none 1 dust 2 smoke 3 fire 4 flash 5 spark)  y roughness 0..1
//                      z churn, noise cells a second  w detail, octaves at quality 2
//   ParticleLookHeat   x heat start  y heat end  z prongs (min * 32 + max)  w rise, map units a second
#define ParticleLookShape(slot) particleDefinitions[slot].spare[0]
#define ParticleLookHeat(slot) particleDefinitions[slot].spare[1]
const int kParticleDefinitionSlots = 512;	// ParticleDefinitionBuffer::SLOTS
const int kLookFire = 3;

// [LOOKS] Map units across one cell of the first noise octave, whatever the particle's size: a
// puff that grows shows more of the same-sized billows, never the same billows stretched.
const float kLookCellUnits = 3.5;

// [LOOKS] THE HEAT RAMP, fixed in the engine so a hot look cannot go pink or green by accident
// ("Engine docs/GPU_PARTICLE_LOOKS_PLAN.md", guardrail 1): soot, deep red, orange, yellow,
// white-hot, and dimmer as it cools. Every key has red >= green >= blue, so every mix between
// keys does too. A definition's colour ramp multiplies it (vParticleColor), which is how plasma
// or BFG fire would keep their colour.
const vec3 kHeatRamp[5] = vec3[5](
	vec3(0.0, 0.0, 0.0),		// 0     soot: gives no light
	vec3(0.42, 0.045, 0.0),		// 0.25  deep red
	vec3(1.0, 0.32, 0.03),		// 0.5   orange
	vec3(1.0, 0.68, 0.22),		// 0.75  yellow
	vec3(1.0, 0.96, 0.86));		// 1     white-hot

vec3 ParticleHeatColor(float heat)
{
	float rampPosition = clamp(heat, 0.0, 1.0) * 4.0;
	int k = min(int(rampPosition), 3);
	return mix(kHeatRamp[k], kHeatRamp[k + 1], rampPosition - float(k));
}

// [LOOKS] Fractal value noise, 0..1: `octaves` of valueNoise, each twice as fine (in time too, so
// small eddies turn over faster) and half as strong. The first octave, already sampled at p, is
// passed in so a caller that needs it again pays for it once.
float ParticleLookFractal(vec3 p, float firstOctave, int octaves)
{
	float total = firstOctave * 0.5;
	float weight = 0.5;
	float amplitude = 0.5;
	for (int i = 1; i < octaves; i++)
	{
		p = p * 2.03 + vec3(17.13, 5.71, 11.37);
		amplitude *= 0.5;
		total += valueNoise(p) * amplitude;
		weight += amplitude;
	}
	return total / weight;
}

// [LOOKS] One particle's generated look at this pixel, as the three numbers the blends use
// (falloff, bodyShade, coverage, as in main) plus glowTint, what the emissive colour is multiplied
// by. The noise is sampled at (corner x size / kLookCellUnits + seed, age x churn): the particle's
// own space, so the shape is the same in both eyes and never swims on screen.
//   dust  a lit body only -- it never glows, whatever its emissive says. Noise tears the round
//         cloud's edge by `roughness`; late in life the edge is eaten away, so it billows, then
//         tears and thins. At quality 2+ two more first-octave samples give a slope on a dome, lit
//         from vParticleLightDir: brighter facing the light, darker away and in the thick core.
//   fire  glows. The noise is twice as tall as wide and streams up by `rise`, making tongues torn
//         more at the top; heat = the life's heat (start to end) x how dense the flame is here,
//         through the heat ramp -- white in the core, red at the tips, cooling over life. Its
//         body (a late alpha ramp: soot) takes the shape untinted.
void ParticleLook(out float falloff, out vec3 bodyShade, out float coverage, out vec3 glowTint)
{
	falloff = 0.0;
	bodyShade = vec3(0.0);
	coverage = 0.0;
	glowTint = vec3(1.0);

	// Every look lives inside the quad's inscribed circle: outside it, nothing, and no noise paid for.
	vec2 q = vParticleCorner;
	float r2 = dot(q, q);
	if (r2 >= 1.0)
		return;

	int slot = clamp(int(vParticleFlipbook.w + 0.5), 0, kParticleDefinitionSlots - 1);
	vec4 lookShape = ParticleLookShape(slot);
	vec4 lookHeat = ParticleLookHeat(slot);
	int kind = int(lookShape.x + 0.5);
	float roughness = clamp(lookShape.y, 0.0, 1.0);
	float churn = lookShape.z;
	int quality = clamp(int(uGpuParticleParams2.z + 0.5), 0, 3);
	int octaves = clamp(int(lookShape.w + 0.5) + quality - 2, 1, 4);

	float lifeFraction = vParticleLife.x;
	float age = vParticleLife.y;
	float halfSize = 0.5 * vParticleLife.w;
	float cells = halfSize / kLookCellUnits;	// noise cells from the centre to the edge
	vec3 seedOffset = vec3(97.0, 57.0, 31.0) * vParticleLife.z;

	float inner = 1.0 - sqrt(r2);	// 1 at the centre, 0 at the edge
	float window = 1.0 - r2;		// 0 at the edge, so the quad never cuts a shape
	float lateLife = lifeFraction * lifeFraction;

	if (kind == kLookFire)
	{
		float rise = lookHeat.w;
		vec3 p = vec3(q.x * cells, (q.y * halfSize - age * rise) / (2.0 * kLookCellUnits), age * churn) + seedOffset;
		float n = ParticleLookFractal(p, valueNoise(p), octaves);
		// A flame narrows toward its top: a teardrop envelope instead of the dot's circle.
		float upper = 0.5 + 0.5 * q.y;
		float flameInner = 1.0 - length(vec2(q.x * mix(0.9, 1.6, upper), q.y));
		float field = flameInner - roughness * (1.0 - n) * (0.45 + 0.75 * upper) - 0.3 * lateLife;
		coverage = smoothstep(0.0, 0.2, field) * window;
		float heat = mix(lookHeat.x, lookHeat.y, lifeFraction) * (0.4 + 0.6 * smoothstep(0.0, 0.55, field));
		glowTint = ParticleHeatColor(heat);
		falloff = coverage;
		bodyShade = vec3(coverage);
		return;
	}

	// DUST (the parser refuses every look this build does not draw).
	vec3 p = vec3(q * cells, age * churn) + seedOffset;
	float firstOctave = valueNoise(p);
	float n = ParticleLookFractal(p, firstOctave, octaves);
	float field = mix(inner, inner + n - 0.5, roughness) - 0.35 * lateLife * (0.4 + roughness);
	// The thinner parts of the noise thin the body too -- never above 1, so the occlusion never
	// passes the alpha ramp (a premultiplied alpha above 1 would darken what is behind).
	coverage = smoothstep(0.0, 0.2, field) * window * (1.0 - 0.7 * roughness * (1.0 - n));

	float shade = 1.0;
	if (quality >= 2 && vParticleLightDir.w > 0.0)
	{
		// Half-Lambert on a dome bent by the noise's slope: 0.55 facing away to 1.45 facing the
		// light, about 1 on average, so the lit body colour from gpuparticles.vp keeps its level.
		const float kSlopeStep = 0.35;
		vec2 slope = (vec2(valueNoise(p + vec3(kSlopeStep, 0.0, 0.0)), valueNoise(p + vec3(0.0, kSlopeStep, 0.0))) - vec2(firstOctave)) / kSlopeStep;
		vec3 surfaceNormal = normalize(vec3(q - roughness * slope, sqrt(window) + 0.1));
		float facing = clamp(0.5 + 0.5 * dot(surfaceNormal, vParticleLightDir.xyz), 0.0, 1.0);
		float thickness = smoothstep(0.35, 1.0, field);
		shade = mix(1.0, (0.55 + 0.9 * facing) * (1.0 - 0.2 * thickness), vParticleLightDir.w);
	}
	bodyShade = vec3(coverage * shade);
}

layout(location = 0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location = 1) out vec4 FragFog;
layout(location = 2) out vec4 FragNormal;
#endif
#ifdef SCENE_LIGHT_MASK
// [LIGHTMASK] The light mask (hw_postprocess.h, PPLightMask): the glow -- the emissive colour this
// pixel adds, heat ramp included -- is emissive light; a lit body is neither class (dust with no glow
// writes 0). Written with the colour's own alpha (falloff when additive, occlusion when
// premultiplied), so the blend treats the amount exactly as it treats the colour it came from.
layout(location = LIGHT_MASK_LOCATION) out vec4 FragLightMask;
#endif
#ifdef SCENE_POST_MASK
// [SCENEMASK] The scene tag (main.fp's block says what this is). Particles of many definitions
// are drawn in ONE draw, so a per-draw tag cannot tell them apart: the tag comes from the
// particle's DEFINITION (`postmask` in PARTICLEDEFS, carried above the PDF_ flags in look.w --
// particledefs.h says why it lives there), and only a definition that sets none falls back to
// the draw's tag. Same rule either way: 0 is nothing special.
#define uPostMask data[uDataIndex].padding1
#define ParticleDefinitionPostMask(slot) (int(particleDefinitions[slot].look.w + 0.5) >> 16)
layout(location = POST_MASK_LOCATION) out vec4 FragPostMask;
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
	// [LOOKS] What the emissive colour is multiplied by here: the heat ramp for fire, else 1.
	vec3 glowTint = vec3(1.0);
	if (vParticleLife.w > 0.0)
	{
		// [LOOKS] A generated look (a look excludes a texture, so the flipbook is never lost).
		ParticleLook(falloff, bodyShade, coverage, glowTint);
	}
	else if (vParticleFlipbook.x < -0.5)
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
		// [ATLASBC7] From the atlas the definition's frames are in: PDF_ATLAS_COMPRESSED (2) in its look.w, read by the slot
		// vParticleFlipbook.w carries, clamped as ParticleLook clamps it. Both atlases hold premultiplied texels, so everything
		// after the two reads is the same for either.
		int atlasSlot = clamp(int(vParticleFlipbook.w + 0.5), 0, kParticleDefinitionSlots - 1);
		vec4 frameNow;
		vec4 frameNext;
		if ((int(particleDefinitions[atlasSlot].look.w + 0.5) & 2) != 0)
		{
			frameNow = textureGrad(ParticleAtlasCompressed, vec3(atlasCoord, vParticleFlipbook.x), atlasCoordDx, atlasCoordDy);
			frameNext = textureGrad(ParticleAtlasCompressed, vec3(atlasCoord, vParticleFlipbook.y), atlasCoordDx, atlasCoordDy);
		}
		else
		{
			frameNow = textureGrad(ParticleAtlas, vec3(atlasCoord, vParticleFlipbook.x), atlasCoordDx, atlasCoordDy);
			frameNext = textureGrad(ParticleAtlas, vec3(atlasCoord, vParticleFlipbook.y), atlasCoordDx, atlasCoordDy);
		}
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
		FragColor = vec4(vParticleBody.rgb * (vParticleBody.a * bodyShade) + vParticleColor.rgb * glowTint * falloff, occlusion);
#ifdef SCENE_LIGHT_MASK
		vec3 hotLight = vParticleColor.rgb * glowTint * falloff;	// [LIGHTMASK] the glow term above
		FragLightMask = vec4(hotLight.r + hotLight.g + hotLight.b, 0.0, 0.0, occlusion);
#endif
	}
	else
	{
		FragColor = vec4(vParticleColor.rgb * glowTint, falloff);
#ifdef SCENE_LIGHT_MASK
		vec3 hotLight = vParticleColor.rgb * glowTint;	// [LIGHTMASK] all of it is glow; the blend applies falloff
		FragLightMask = vec4(hotLight.r + hotLight.g + hotLight.b, 0.0, 0.0, falloff);
#endif
	}

#ifdef SCENE_POST_MASK
	// [SCENEMASK] One stamp for both branches above.
	{
		int maskSlot = clamp(int(vParticleFlipbook.w + 0.5), 0, kParticleDefinitionSlots - 1);
		int maskTag = ParticleDefinitionPostMask(maskSlot);
		if (maskTag == 0) maskTag = uPostMask;
		FragPostMask = vec4(float(clamp(maskTag, 0, 255)) * (1.0 / 255.0), 0.0, 0.0, 1.0);
	}
#endif
#ifdef GBUFFER_PASS
	// Zero with zero alpha: under additive blending this adds nothing to the fog
	// and normal attachments, and [2d] under premultiplied blending (dst x (1 - 0))
	// it leaves them as they are too.
	FragFog = vec4(0.0);
	FragNormal = vec4(0.0);
#endif
}
