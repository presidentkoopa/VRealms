/*
** blur.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2018-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SourceTexture;

#if defined(BLUR_STEPPED)
// [BLOOMSTEP] The wide bloom blur, used only when gl_bloom_step > 1 (E3 in
// "Engine docs/REVIEW_BLOOM_PLAN.md"). The same seven Gaussian taps, TexelStep texels
// apart instead of one. A tap is not a single read: it is the mean of ReadsPerTap
// linear reads spread evenly over its own TexelStep-wide stretch, at most two texels
// apart, so no texel between the taps is skipped. One read per tap leaves ghost
// copies of a small bright source -- a laser core -- after the level chain
// (simulated in "Engine docs/BLOOM_STEP1_IMPL_NOTES.md"). The weights come from
// PPBloom::ComputeBlurSamplesStepped. With BLUR_STEPPED undefined this block is not
// compiled, and the two variants below are today's exactly.
vec4 SteppedTap(vec2 tapAxis, float tapCentre)
{
	float readSpacing = TexelStep / float(ReadsPerTap);
	float firstRead = tapCentre - 0.5 * TexelStep + 0.5 * readSpacing;
	vec4 total = vec4(0.0);
	for (int r = 0; r < ReadsPerTap; r++)
		total += texture(SourceTexture, TexCoord + tapAxis * (firstRead + float(r) * readSpacing));
	return total / float(ReadsPerTap);
}
#endif

void main()
{
#if defined(BLUR_STEPPED)
	vec2 texelSize = 1.0 / vec2(textureSize(SourceTexture, 0));
#if defined(BLUR_HORIZONTAL)
	vec2 tapAxis = vec2(texelSize.x, 0.0);
#else
	vec2 tapAxis = vec2(0.0, texelSize.y);
#endif
	FragColor =
		SteppedTap(tapAxis, 0.0) * SampleWeights0 +
		SteppedTap(tapAxis, TexelStep) * SampleWeights1 +
		SteppedTap(tapAxis, -TexelStep) * SampleWeights2 +
		SteppedTap(tapAxis, 2.0 * TexelStep) * SampleWeights3 +
		SteppedTap(tapAxis, -2.0 * TexelStep) * SampleWeights4 +
		SteppedTap(tapAxis, 3.0 * TexelStep) * SampleWeights5 +
		SteppedTap(tapAxis, -3.0 * TexelStep) * SampleWeights6;
#elif defined(BLUR_HORIZONTAL)
	FragColor =
		textureOffset(SourceTexture, TexCoord, ivec2( 0, 0)) * SampleWeights0 +
		textureOffset(SourceTexture, TexCoord, ivec2( 1, 0)) * SampleWeights1 +
		textureOffset(SourceTexture, TexCoord, ivec2(-1, 0)) * SampleWeights2 +
		textureOffset(SourceTexture, TexCoord, ivec2( 2, 0)) * SampleWeights3 +
		textureOffset(SourceTexture, TexCoord, ivec2(-2, 0)) * SampleWeights4 +
		textureOffset(SourceTexture, TexCoord, ivec2( 3, 0)) * SampleWeights5 +
		textureOffset(SourceTexture, TexCoord, ivec2(-3, 0)) * SampleWeights6;
#else
	FragColor =
		textureOffset(SourceTexture, TexCoord, ivec2(0, 0)) * SampleWeights0 +
		textureOffset(SourceTexture, TexCoord, ivec2(0, 1)) * SampleWeights1 +
		textureOffset(SourceTexture, TexCoord, ivec2(0,-1)) * SampleWeights2 +
		textureOffset(SourceTexture, TexCoord, ivec2(0, 2)) * SampleWeights3 +
		textureOffset(SourceTexture, TexCoord, ivec2(0,-2)) * SampleWeights4 +
		textureOffset(SourceTexture, TexCoord, ivec2(0, 3)) * SampleWeights5 +
		textureOffset(SourceTexture, TexCoord, ivec2(0,-3)) * SampleWeights6;
#endif
}
