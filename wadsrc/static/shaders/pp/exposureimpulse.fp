/*
** exposureimpulse.fp
**
** [EXPOSUREIMPULSE] Flash blindness: the wash ("Engine docs/SENSORY_IMPULSES_PLAN.md" 2d; the C++ is PPExposureImpulse,
** hw_postprocess.h / .cpp; the numbers come from hw_exposureimpulsecore.h through hw_exposureimpulse.cpp).
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** EXPOSURE_IMPULSE_LATCH  The burst's darkness: the exposure meter's 1x1 value copied into the latch texture.
**
** Otherwise THE WASH, last in Pass1, after bloom, over the whole screen into the next pipeline image:
**   darkness  0 bright .. 1 dark: the latched meter value between the lit and dark references (METER), or the game side's
**             guess from the view sector's light; then smoothstepped
**   wash      Amount x mix(DarkFloor, 1, darkness): a bright room keeps DarkFloor of it
**   HOLD_BEAMS  wash x (1 - the pixel's pinned share): a pixel of beam light is returned exactly as read
**   picture   toward grey by Desaturate x wash, brighter by 1 + Gain x wash, plus this eye's bloom x Glare x wash (GLARE),
**             plus the haze VeilTint x Veil x wash
** A pixel with no wash (Amount 0, a pure beam pixel under HOLD_BEAMS, a NaN) and every pixel outside the scene rectangle
** (a letterbox) is written exactly as read. Alpha is kept.
**
** SAMPLERS. Each sampler name has ONE binding number in every variant: the GL 3.3 path strips every sampler binding
** declaration in the text -- compiled or not -- and binds by name. A sampler whose slot moves with the variant therefore has
** a name per slot, read through one alias.
**
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D InputTexture;

#if defined(EXPOSURE_IMPULSE_LATCH)

void main()
{
	FragColor = vec4(texture(InputTexture, vec2(0.5)).x, 0.0, 0.0, 1.0);
}

#else

#if defined(EXPOSURE_IMPULSE_METER)
layout(binding=1) uniform sampler2D LatchTexture;
#endif

#if defined(EXPOSURE_IMPULSE_GLARE)
#if defined(EXPOSURE_IMPULSE_METER)
layout(binding=2) uniform sampler2D BloomTextureAt2;
#define BloomTexture BloomTextureAt2
#else
layout(binding=1) uniform sampler2D BloomTextureAt1;
#define BloomTexture BloomTextureAt1
#endif
#endif

#if defined(EXPOSURE_IMPULSE_HOLD_BEAMS)
#if defined(EXPOSURE_IMPULSE_METER) && defined(EXPOSURE_IMPULSE_GLARE)
layout(binding=3) uniform sampler2D LightMaskTextureAt3;
#define LightMaskTexture LightMaskTextureAt3
#elif defined(EXPOSURE_IMPULSE_METER) || defined(EXPOSURE_IMPULSE_GLARE)
layout(binding=2) uniform sampler2D LightMaskTextureAt2;
#define LightMaskTexture LightMaskTextureAt2
#else
layout(binding=1) uniform sampler2D LightMaskTextureAt1;
#define LightMaskTexture LightMaskTextureAt1
#endif

// bloomextract.fp's PinnedShare, token for token (vkcheck_si_l.py SH): the pixel's share of pinned (beam) light, snapped to
// exactly 0 or 1 within 1/1024, so a pure beam pixel takes no wash at all.
float PinnedShare(vec3 sceneColour, vec2 amounts)
{
	float total = sceneColour.r + sceneColour.g + sceneColour.b;
	if (!(total >= 1.0e-4))
		return 0.0;
	float share = amounts.g / total;
	share = share > 0.0 ? (share < 1.0 ? share : 1.0) : 0.0;
	return share < 1.0 / 1024.0 ? 0.0 : (share > 1.0 - 1.0 / 1024.0 ? 1.0 : share);
}
#endif

void main()
{
	vec4 sceneColour = texture(InputTexture, TexCoord);

	// The scene's own coordinate inside the pipeline image; outside it the image passes through.
	vec2 sceneCoord = (TexCoord - Offset) / Scale;
	if (!(sceneCoord.x >= 0.0 && sceneCoord.y >= 0.0 && sceneCoord.x <= 1.0 && sceneCoord.y <= 1.0))
	{
		FragColor = sceneColour;
		return;
	}

#if defined(EXPOSURE_IMPULSE_METER)
	float darkness = clamp((texture(LatchTexture, vec2(0.5)).x - LitExposure) / (DarkExposure - LitExposure), 0.0, 1.0);
#else
	float darkness = clamp(FallbackDarkness, 0.0, 1.0);
#endif
	darkness = darkness * darkness * (3.0 - 2.0 * darkness);

	float wash = Amount * mix(DarkFloor, 1.0, darkness);
#if defined(EXPOSURE_IMPULSE_HOLD_BEAMS)
	wash *= 1.0 - PinnedShare(sceneColour.rgb, texture(LightMaskTexture, TexCoord).rg);
#endif

	if (!(wash > 0.0))
	{
		FragColor = sceneColour;
		return;
	}

	float luma = dot(sceneColour.rgb, vec3(0.2126, 0.7152, 0.0722));
	vec3 colour = mix(sceneColour.rgb, vec3(luma), Desaturate * wash);
	colour *= 1.0 + Gain * wash;
#if defined(EXPOSURE_IMPULSE_GLARE)
	colour += texture(BloomTexture, sceneCoord).rgb * (Glare * wash);
#endif
	colour += VeilTint * (Veil * wash);
	FragColor = vec4(colour, sceneColour.a);
}

#endif
