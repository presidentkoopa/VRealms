
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;        // the image so far (linear)
layout(binding=1) uniform sampler2D HeatOffsetTexture;   // heatoffset.fp's sum, half resolution (linear)

// MULTISAMPLE is defined for the WarpMS variant (hw_postprocess.h), picked when
// gl_multisample > 1, as volumetricbeam.fp and heatmap.fp do.
#if defined(MULTISAMPLE)
layout(binding=2) uniform sampler2DMS DepthTexture;
#else
layout(binding=2) uniform sampler2D DepthTexture;
#endif

// ============================================================================
// [HEATREFRACTION] HEAT SHIMMER, PASS 2 OF 2: BEND THE IMAGE.
// ("Engine docs/FLAME_ENGINE_PLAN.md" F2; the C++ is PPHeatRefraction.)
//
// Once per eye, only when a heat source was drawn this eye. Reads the current
// image and writes the next one, over the whole screen viewport like the lens
// distortion pass, so no texel of the next image is left unwritten.
//
// TexCoord spans the screen viewport, which is the pipeline image's and the depth
// texture's UV space; the scene viewport sits inside it at SceneOffset, SceneScale
// (the pair bloom, lineardepth.fp and the beam use), and the offset texture covers
// the scene viewport only.
//
// Two things keep foreground edges clean where the half-resolution offsets are
// smeared by filtering:
//   - a pixel nearer than where its heat begins is left alone (a hand in front of
//     a flame stays sharp to its edge);
//   - a bent sample that lands on something nearer than the heat is refused, so a
//     hand's colour is never dragged into the air beside it.
// ============================================================================

float LinearDepthAt(vec2 uv)
{
#if defined(MULTISAMPLE)
	ivec2 depthSize = textureSize(DepthTexture);
	ivec2 depthTexel = clamp(ivec2(uv * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
	float rawDepth = texelFetch(DepthTexture, depthTexel, 0).x;
#else
	float rawDepth = texture(DepthTexture, uv).x;
#endif
	return 1.0 / (clamp(rawDepth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
}

void main()
{
	vec4 unbent = texture(InputTexture, TexCoord);
	FragColor = unbent;

	vec2 sceneUV = (TexCoord - SceneOffset) / SceneScale;
	if (sceneUV.x < 0.0 || sceneUV.y < 0.0 || sceneUV.x > 1.0 || sceneUV.y > 1.0) return;

	vec4 heat = texture(HeatOffsetTexture, sceneUV);
	if (heat.a <= 0.0001) return;

	// The summed shift of every source over this pixel, capped so a huge or stacked
	// source smears no further than MaxShift (scene UV units).
	vec2 shift = heat.rg;
	float shiftLen = length(shift);
	if (shiftLen > MaxShift) shift *= MaxShift / shiftLen;

	float entryDepth = heat.b / heat.a;
	if (LinearDepthAt(TexCoord) < entryDepth - DepthMargin) return;

	vec2 bentUV = clamp(TexCoord + shift * SceneScale, SceneOffset, SceneOffset + SceneScale);
	if (LinearDepthAt(bentUV) < entryDepth - DepthMargin) return;

#if defined(CHROMATIC)
	// [SHOCKWAVE] THE COLOUR FRINGE -- the WarpChroma programs (hw_postprocess.h), run only on a frame whose blast ripples ask
	// for one. Red comes from further along the bend and blue from less far, by Chroma of the shift; the depth tests above
	// used this green tap. A red or blue tap that lands on something nearer than the air keeps the green tap's colour, so a
	// hand's edge never leaks one colour into the air beside it.
	FragColor = texture(InputTexture, bentUV);
	vec2 redUV = clamp(TexCoord + shift * (1.0 + Chroma) * SceneScale, SceneOffset, SceneOffset + SceneScale);
	vec2 blueUV = clamp(TexCoord + shift * (1.0 - Chroma) * SceneScale, SceneOffset, SceneOffset + SceneScale);
	if (LinearDepthAt(redUV) >= entryDepth - DepthMargin) FragColor.r = texture(InputTexture, redUV).r;
	if (LinearDepthAt(blueUV) >= entryDepth - DepthMargin) FragColor.b = texture(InputTexture, blueUV).b;
#else
	FragColor = texture(InputTexture, bentUV);
#endif
}
