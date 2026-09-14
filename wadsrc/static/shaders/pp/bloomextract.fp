/*
** bloomextract.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2016 Magnus Norddahl
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D ExposureTexture;

#if defined(BLOOM_EXTRACT_SHARE) || defined(BLOOM_EXTRACT_DUAL)
// ============================================================================
// [PINNEDBLOOM] THE EXTRACT WEIGHED BY THE LIGHT MASK ("Engine docs/
// EMISSIVE_BLOOM_PLAN.md" 2f; the C++ is PPBloom::RenderBloom and PPBloomPlan).
//
// The light mask (lightmaskdebug.fp says what it holds) records per pixel how
// much of the colour is PINNED light -- beam light, while gl_bloom_pin_beams is
// on. That share of a pixel blooms with the pinned look (the gl_bloom_pin_*
// settings) and the rest of it with the live look, so a preset or a scripted
// bloom override changes the room's glow and never the beams'.
//
// BLOOM_EXTRACT_SHARE: one look times one share -- WeightClass 1 the pinned
//   share, 0 the rest (1 - pinned). Each of PPBloom's two chains runs one.
// BLOOM_EXTRACT_DUAL: both looks in one extract, each with its own tint, for
//   when they differ only before the blur (threshold, knee, exposure, tint and
//   intensity). The final combine then adds a neutral tint: tint is a
//   per-channel scale and every tap after this is linear per channel, so moving
//   it here changes nothing but float rounding.
//
// ExtractBloom is today's extract (the #else branch), line for line, so at a
// pixel that is all pinned light the pinned look's extract is today's at those
// settings. Without either define this block is not compiled.
// ============================================================================

#if defined(BLOOM_EXTRACT_DUAL)
layout(binding=2) uniform sampler2D PinnedExposureTexture;
layout(binding=3) uniform sampler2D LightMaskTexture;
#else
layout(binding=2) uniform sampler2D LightMaskTexture;
#endif

vec3 ExtractBloom(vec3 sceneColour, float exposureAdjustment, float threshold, float knee)
{
	vec3 c = (sceneColour + vec3(0.001)) * exposureAdjustment;

	// Judge brightness by the strongest channel, so a saturated colour blooms
	// on its own terms rather than needing to be bright in all three.
	float brightness = max(c.r, max(c.g, c.b));

	float contribution;
	if (knee > 0.0001)
	{
		float soft = brightness - threshold + knee;
		soft = clamp(soft, 0.0, 2.0 * knee);
		soft = soft * soft / (4.0 * knee);
		contribution = max(soft, brightness - threshold) / max(brightness, 0.0001);
	}
	else
	{
		contribution = max(brightness - threshold, 0.0) / max(brightness, 0.0001);
	}

	return c * contribution;
}

// The pinned share of one sample: its amount of pinned light over the colour's
// r + g + b, both read at the same UV with the same filter, so a filtered
// sample's share is exact. 0..1 with a NaN landing on 0, and snapped to exactly
// 0 or 1 within 1/1024, so half-float noise never leaks a trace of the other
// look onto a pixel that is all beam or all room.
float PinnedShare(vec3 sceneColour, vec2 amounts)
{
	float total = sceneColour.r + sceneColour.g + sceneColour.b;
	if (!(total >= 1.0e-4))
		return 0.0;
	float share = amounts.g / total;
	share = share > 0.0 ? (share < 1.0 ? share : 1.0) : 0.0;
	return share < 1.0 / 1024.0 ? 0.0 : (share > 1.0 - 1.0 / 1024.0 ? 1.0 : share);
}

void main()
{
	vec2 uv = Offset + TexCoord * Scale;
	vec3 sceneColour = texture(SceneTexture, uv).rgb;
	float pinned = PinnedShare(sceneColour, texture(LightMaskTexture, uv).rg);

#if defined(BLOOM_EXTRACT_DUAL)
	// A pixel that is all one class takes that look's extract alone, not a sum with
	// a zero: exact, and an overflow in the other look cannot reach it.
	vec3 bloom;
	if (pinned >= 1.0)
	{
		bloom = ExtractBloom(sceneColour, texture(PinnedExposureTexture, vec2(0.5)).x, PinThreshold, PinKnee) * PinTint;
	}
	else if (pinned <= 0.0)
	{
		bloom = ExtractBloom(sceneColour, texture(ExposureTexture, vec2(0.5)).x, RestThreshold, RestKnee) * RestTint;
	}
	else
	{
		vec3 restBloom = ExtractBloom(sceneColour, texture(ExposureTexture, vec2(0.5)).x, RestThreshold, RestKnee) * RestTint;
		vec3 pinnedBloom = ExtractBloom(sceneColour, texture(PinnedExposureTexture, vec2(0.5)).x, PinThreshold, PinKnee) * PinTint;
		bloom = restBloom * (1.0 - pinned) + pinnedBloom * pinned;
	}
	FragColor = vec4(bloom, 1.0);
#else
	float weight = WeightClass == 1 ? pinned : 1.0 - pinned;
	if (weight <= 0.0)
		FragColor = vec4(0.0, 0.0, 0.0, 1.0);
	else
		FragColor = vec4(ExtractBloom(sceneColour, texture(ExposureTexture, vec2(0.5)).x, Threshold, Knee) * weight, 1.0);
#endif
}

#else
// [BB] Soft-knee threshold.
//
// The original was a hard cutoff: subtract 1.0 and clamp. A pixel just under
// the line contributed nothing and a pixel just over it contributed fully, so
// anything drifting across the threshold POPPED. With glow that pulses and
// beams that sweep, things cross that line constantly and the popping is the
// first thing you notice.
//
// The knee rolls the transition over a range below the threshold using a
// quadratic, so bloom eases in. Knee 0 restores the old hard behaviour.
void main()
{
	float exposureAdjustment = texture(ExposureTexture, vec2(0.5)).x;
	vec3 c = (texture(SceneTexture, Offset + TexCoord * Scale).rgb + vec3(0.001)) * exposureAdjustment;

	// Judge brightness by the strongest channel, so a saturated colour blooms
	// on its own terms rather than needing to be bright in all three.
	float brightness = max(c.r, max(c.g, c.b));

	float contribution;
	if (Knee > 0.0001)
	{
		float soft = brightness - Threshold + Knee;
		soft = clamp(soft, 0.0, 2.0 * Knee);
		soft = soft * soft / (4.0 * Knee);
		contribution = max(soft, brightness - Threshold) / max(brightness, 0.0001);
	}
	else
	{
		contribution = max(brightness - Threshold, 0.0) / max(brightness, 0.0001);
	}

	FragColor = vec4(c * contribution, 1.0);
}
#endif
