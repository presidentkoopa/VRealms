layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// MULTISAMPLE is defined for the MS variant (hw_postprocess.h, PPCustomShaders), picked when gl_multisample > 1: the
// scene mask is then multisampled and is read with texelFetch, as customdepth.fp reads the scene depth.
// NO_SCENE_MASK is defined for the variant used when this eye's scene drew no mask; its binding is the current
// pipeline image, which it reads and throws away so the uniforms and the sampler are used exactly as in the others.
#if defined(MULTISAMPLE) && !defined(NO_SCENE_MASK)
layout(binding=0) uniform sampler2DMS MaskTexture;
#else
layout(binding=0) uniform sampler2D MaskTexture;
#endif

// ============================================================================
// [SCENEMASK] THE SCENE MASK FOR GLDEFS CUSTOM POST-PROCESS SHADERS.
//
// A custom shader that names a texture "SceneMask" declares it as a plain sampler2D and samples it at its own
// TexCoord, which runs over the screen viewport. This pass draws over the whole viewport of a single-sample texture
// that size and writes, in the red channel of each texel, the tag the scene pass stamped for the pixel under it:
// remapped from the scene's own viewport (SceneScale/SceneOffset) and, when the scene is multisampled, FROM SAMPLE 0.
//
// Sample 0 and not an average, and not a maximum. A tag is a name, not a quantity: the average of 3 and 9 is 6,
// which no fragment wrote, and the maximum is 9, which no fragment wrote at that pixel either. Sample 0 is a tag
// something really drew there. The cost is that an edge pixel takes one of its fragments' tags rather than blending
// them, which is the only honest answer a single byte can give.
//
// The shader reads the tag back as `texture(SceneMask, TexCoord).r * 255.0`. 0 is "nothing special" and is what
// every untagged pixel, every eye without a mask, and every frame with the mask off all read.
//
// Run once per eye before the first custom shader that needs it (PPCustomShaders::Run).
// ============================================================================

void main()
{
#if defined(MULTISAMPLE) && !defined(NO_SCENE_MASK)
	ivec2 maskSize = textureSize(MaskTexture);
#else
	ivec2 maskSize = textureSize(MaskTexture, 0);
#endif
	vec2 maskUV = SceneOffset + TexCoord * SceneScale;
	ivec2 texel = clamp(ivec2(maskUV * vec2(maskSize)), ivec2(0), maskSize - ivec2(1));
	float maskTag = texelFetch(MaskTexture, texel, 0).x;
#if defined(NO_SCENE_MASK)
	maskTag = 0.0;	// this eye's scene drew no mask: every pixel is "nothing special"
#endif
	FragColor = vec4(maskTag, 0.0, 0.0, 1.0);
}
