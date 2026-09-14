
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SmokeTexture;		// the march, or the first blur draw: rgb light, a transmittance
layout(binding=1) uniform sampler2D SmokeDepthTexture;	// smokedepth.fp: the depth each texel marched to

// ============================================================================
// [SMOKEVOLUME] THE SMOKE VOLUME'S DRAWING, PASS 3 OF 4: A BLUR THAT KEEPS TO ITS DEPTH.
// ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c; the C++ is PPSmokeVolume.)
//
// Five taps along one axis (BLUR_HORIZONTAL or BLUR_VERTICAL; PPSmokeVolume draws both), binomial
// weights 1 4 6 4 1. Each tap's weight is cut by how far its march depth is from this texel's, so the
// march's per-pixel sample offsets smooth out without haze bleeding from a far wall onto a near hand
// or back. Every tap holds linear quantities (light and transmittance), so a weighted mean is an exact
// blend of them.
//
// A texel whose five taps are all exactly clear, (0, 0, 0, 1), stays exactly clear. It is returned as
// that constant rather than as a mean that rounding could move off it, so the composite still leaves
// the pixel alone.
// ============================================================================

float DepthWeight(float centre, float other)
{
	float relative = abs(other - centre) / max(min(other, centre), 1.0);
	return 1.0 / (1.0 + 400.0 * relative * relative);
}

void main()
{
	ivec2 size = textureSize(SmokeTexture, 0);
	ivec2 here = ivec2(gl_FragCoord.xy);
#if defined(BLUR_HORIZONTAL)
	ivec2 axis = ivec2(1, 0);
#else
	ivec2 axis = ivec2(0, 1);
#endif
	float centreDepth = texelFetch(SmokeDepthTexture, here, 0).r;

	const vec4 clearTap = vec4(0.0, 0.0, 0.0, 1.0);
	vec4 sum = vec4(0.0);
	float weightSum = 0.0;
	bool anySmoke = false;
	for (int i = -2; i <= 2; i++)
	{
		ivec2 tap = clamp(here + axis * i, ivec2(0), size - ivec2(1));
		vec4 smoke = texelFetch(SmokeTexture, tap, 0);
		if (smoke != clearTap)
			anySmoke = true;
		float binomial = (i == 0) ? 6.0 : ((i == -1 || i == 1) ? 4.0 : 1.0);
		float w = binomial * DepthWeight(centreDepth, texelFetch(SmokeDepthTexture, tap, 0).r);
		sum += smoke * w;
		weightSum += w;
	}
	FragColor = anySmoke ? sum / weightSum : clearTap;
}
