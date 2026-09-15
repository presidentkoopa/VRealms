
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// MULTISAMPLE is defined for the MS variant (hw_postprocess.h, PPCustomShaders), picked when gl_multisample > 1: the
// scene depth is then multisampled and is read with texelFetch, as heatoffset.fp and smokedepth.fp read it.
#if defined(MULTISAMPLE)
layout(binding=0) uniform sampler2DMS DepthTexture;
#else
layout(binding=0) uniform sampler2D DepthTexture;
#endif

// ============================================================================
// [CUSTOMDEPTH] THE SCENE DEPTH FOR GLDEFS CUSTOM POST-PROCESS SHADERS.
//
// A custom shader that names a texture "SceneDepth" declares it as a plain sampler2D and samples it at its own
// TexCoord, which runs over the screen viewport. This pass draws over the whole viewport of a single-sample R32F
// texture that size and writes, for each texel, the raw [0,1] window depth of the scene pixel under it: remapped from
// the scene's own viewport (SceneScale/SceneOffset) and, when the scene is multisampled, from sample 0.
// Run once per eye before the first custom shader that needs it (PPCustomShaders::Run).
// ============================================================================

void main()
{
#if defined(MULTISAMPLE)
	ivec2 depthSize = textureSize(DepthTexture);
#else
	ivec2 depthSize = textureSize(DepthTexture, 0);
#endif
	vec2 depthUV = SceneOffset + TexCoord * SceneScale;
	ivec2 texel = clamp(ivec2(depthUV * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
	FragColor = vec4(texelFetch(DepthTexture, texel, 0).x, 0.0, 0.0, 1.0);
}
