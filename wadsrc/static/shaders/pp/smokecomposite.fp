
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SmokeTexture;		// the blurred march, half resolution: rgb light, a transmittance
layout(binding=1) uniform sampler2D SmokeDepthTexture;	// smokedepth.fp: the depth each half-resolution texel marched to
// MULTISAMPLE: the multisampled scene depth, read with texelFetch (as smokedepth.fp reads it).
#if defined(MULTISAMPLE)
layout(binding=2) uniform sampler2DMS DepthTexture;
#else
layout(binding=2) uniform sampler2D DepthTexture;
#endif

// ============================================================================
// [SMOKEVOLUME] THE SMOKE VOLUME'S DRAWING, PASS 4 OF 4: UP TO FULL RESOLUTION AND ONTO THE IMAGE.
// ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c; the C++ is PPSmokeVolume.)
//
// Drawn over the scene viewport onto the current image with the PREMULTIPLIED blend
// (PPRenderState::SetPremultipliedAlphaBlend). The output is (light, 1 - T), so
//     image = light + image x T
// which is the composite "Engine docs/EMISSIVE_BLOOM_PLAN.md" requires of 13c: linear, premultiplied,
// the transmittance kept per pixel. Nothing here reads the image back.
//
// THE UPSAMPLE takes the four half-resolution texels around this pixel. Each is weighted by its
// bilinear share and by how close the depth it marched to is to this pixel's own depth. A pixel on a
// hand takes the march that stopped at the hand, and a pixel on the wall behind it takes the march that
// went on to the wall (smokedepth.fp gives every 2x2 neighbourhood both).
//
// A pixel whose four texels are all exactly clear is DISCARDED, so it is left exactly as it was. That
// is what keeps everything outside the smoke, lasers included, the image it was.
//
// LIGHT_MASK_CARRY (hw_postprocess.h, PPLightMask): the same weights and the same T, onto the light
// mask with the same blend, adding no light of either class. Every mask amount is therefore dimmed by
// exactly the T the colour is. The emissive air light E4 counts and the pinned beam scatter 13e adds
// are to go into its r and g.
// ============================================================================

float LinearDepthAt(vec2 uv)
{
#if defined(MULTISAMPLE)
	ivec2 depthSize = textureSize(DepthTexture);
#else
	ivec2 depthSize = textureSize(DepthTexture, 0);
#endif
	ivec2 depthTexel = clamp(ivec2(uv * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
	float rawDepth = texelFetch(DepthTexture, depthTexel, 0).x;
	return 1.0 / (clamp(rawDepth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
}

void main()
{
	const vec4 clearTexel = vec4(0.0, 0.0, 0.0, 1.0);

	ivec2 smokeSize = textureSize(SmokeTexture, 0);
	vec2 at = TexCoord * vec2(smokeSize) - vec2(0.5);
	vec2 corner = floor(at);
	vec2 along = at - corner;
	ivec2 cornerTexel = ivec2(corner);

	float z = LinearDepthAt(SceneOffset + TexCoord * SceneScale);

	vec4 sum = vec4(0.0);
	float weightSum = 0.0;
	bool anySmoke = false;
	for (int j = 0; j < 2; j++)
	{
		for (int i = 0; i < 2; i++)
		{
			ivec2 texel = clamp(cornerTexel + ivec2(i, j), ivec2(0), smokeSize - ivec2(1));
			vec4 smoke = texelFetch(SmokeTexture, texel, 0);
			if (smoke != clearTexel)
				anySmoke = true;
			float marched = texelFetch(SmokeDepthTexture, texel, 0).r;
			float bilinear = (i == 0 ? 1.0 - along.x : along.x) * (j == 0 ? 1.0 - along.y : along.y) + 0.001;
			float relative = abs(marched - z) / max(min(marched, z), 1.0);
			float w = bilinear / (0.0004 + relative * relative);
			sum += smoke * w;
			weightSum += w;
		}
	}
	if (!anySmoke)
		discard;

	vec4 blended = sum / weightSum;
	float opacity = clamp(1.0 - blended.a, 0.0, 1.0);
#if defined(LIGHT_MASK_CARRY)
	FragColor = vec4(0.0, 0.0, 0.0, opacity);
#else
	FragColor = vec4(max(blended.rgb, vec3(0.0)), opacity);
#endif
}
