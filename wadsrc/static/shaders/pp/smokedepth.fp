
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// MULTISAMPLE is defined for the DepthShaderMS variant (hw_postprocess.h, PPSmokeVolume), picked when
// gl_multisample > 1: the scene depth is then multisampled and is read with texelFetch, as heatoffset.fp
// and volumetricbeam.fp read it.
#if defined(MULTISAMPLE)
layout(binding=0) uniform sampler2DMS DepthTexture;
#else
layout(binding=0) uniform sampler2D DepthTexture;
#endif

// ============================================================================
// [SMOKEVOLUME] THE SMOKE VOLUME'S DRAWING, PASS 1 OF 4: THE DEPTH IT DRAWS AGAINST.
// ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c; the C++ is PPSmokeVolume.)
//
// Drawn over the whole viewport of a half-resolution texture. Each texel takes the linear view depth
// of one of the four full-resolution pixels under it: the NEAREST on one colour of a checkerboard and
// the FARTHEST on the other.
//
// The march (smokemarch.fp) marches each half-resolution pixel to exactly this depth, and the
// composite (smokecomposite.fp) compares every full-resolution pixel against it. Every 2x2
// neighbourhood therefore offers both a near and a far candidate, so a hand's edge and the wall behind
// it each find a march that stopped where they are.
//
// Both this pass and the composite read a pixel's depth with texelFetch at the same texel, so a
// full-resolution pixel's depth is bit for bit one of the values this pass chose from.
// ============================================================================

float LinearDepthAt(ivec2 texel)
{
	float rawDepth = texelFetch(DepthTexture, texel, 0).x;
	return 1.0 / (clamp(rawDepth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
}

void main()
{
#if defined(MULTISAMPLE)
	ivec2 depthSize = textureSize(DepthTexture);
#else
	ivec2 depthSize = textureSize(DepthTexture, 0);
#endif

	// A quarter of a half-resolution texel either way: the centres of the four pixels under it.
	vec2 quarter = 0.25 * vec2(abs(dFdx(TexCoord.x)), abs(dFdy(TexCoord.y)));
	float nearest = 1e30;
	float farthest = 0.0;
	for (int j = 0; j < 2; j++)
	{
		for (int i = 0; i < 2; i++)
		{
			vec2 sceneUV = TexCoord + quarter * vec2(float(i * 2 - 1), float(j * 2 - 1));
			vec2 depthUV = SceneOffset + sceneUV * SceneScale;
			ivec2 texel = clamp(ivec2(depthUV * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
			float z = LinearDepthAt(texel);
			nearest = min(nearest, z);
			farthest = max(farthest, z);
		}
	}

	int checker = (int(gl_FragCoord.x) + int(gl_FragCoord.y)) & 1;
	FragColor = vec4(checker == 0 ? nearest : farthest, 0.0, 0.0, 1.0);
}
