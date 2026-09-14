layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;       // the image bloom would read
layout(binding=1) uniform sampler2D LightMaskTexture;   // the light mask carried with it

// ============================================================================
// [LIGHTMASK] THE LIGHT MASK, SHOWN (r_lightmask_debug; "Engine docs/
// EMISSIVE_BLOOM_PLAN.md" 2h; the C++ is PPLightMask::RenderDebug).
//
// The mask holds, per pixel, AMOUNTS of light: R emissive light, G pinned light
// (today beam light), each the r + g + b of that light in the colour's own units.
// A pixel's SHARE of a class is its amount over the colour's r + g + b -- the
// number E6b's pinned bloom and E4's emissive-only bloom weigh the extract with.
// Drawn in bloom's place, so the colour here is the one bloom would have read.
//
// DebugMode 1: the scene in dim grey, emissive share added in red and pinned share
//              in green (a pure beam pixel is pure green).
// DebugMode 2: the pinned share alone, green on black.
//
// Both textures share the pipeline image's UV space (the scene transfer puts the
// colour and the mask at the same texels), so one TexCoord reads both.
// ============================================================================

void main()
{
	vec4 sceneColour = texture(InputTexture, TexCoord);
	vec2 amounts = texture(LightMaskTexture, TexCoord).rg;
	float total = sceneColour.r + sceneColour.g + sceneColour.b;

	float pinnedShare = 0.0;
	float emissiveShare = 0.0;
	if (total > 1.0e-4)
	{
		pinnedShare = clamp(amounts.g / total, 0.0, 1.0);
		emissiveShare = clamp(amounts.r / total, 0.0, 1.0 - pinnedShare);
	}

	if (DebugMode == 2)
	{
		FragColor = vec4(0.0, pinnedShare, 0.0, 1.0);
	}
	else
	{
		float grey = clamp(total / 3.0, 0.0, 1.0) * 0.35;
		float marked = pinnedShare + emissiveShare;
		FragColor = vec4(vec3(grey) * (1.0 - marked) + vec3(emissiveShare, pinnedShare, 0.0), 1.0);
	}
}
