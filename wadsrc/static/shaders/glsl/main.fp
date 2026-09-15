/*
** main.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location = 0) in vec4 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 pixelpos;
layout(location = 3) in vec3 glowdist;
layout(location = 4) in vec3 gradientdist;
layout(location = 5) in vec4 vWorldNormal;
layout(location = 6) in vec4 vEyeNormal;
layout(location = 9) in vec3 vLightmap;

#ifdef NO_CLIPDISTANCE_SUPPORT
layout(location = 7) in vec4 ClipDistanceA;
layout(location = 8) in vec4 ClipDistanceB;
#endif

layout(location=0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location=1) out vec4 FragFog;
layout(location=2) out vec4 FragNormal;
#endif
#ifdef SCENE_LIGHT_MASK
//
// [LIGHTMASK] THE LIGHT MASK ("Engine docs/EMISSIVE_BLOOM_PLAN.md" 2c-2d; the C++ is
// PPLightMask in hw_postprocess.h). Only the Vulkan mask programs define SCENE_LIGHT_MASK
// (and LIGHT_MASK_LOCATION), so every other program of this lump is exactly what it was.
//
// FragLightMask = (emissive amount, pinned amount, 0, FragColor.a). An amount is the r + g + b
// of that class's light in FragColor. The bookkeeping below follows the colour through every
// operation and never touches it:
//   - an additive term of a class adds its own rgb (or r + g + b) to that class;
//   - an additive term of neither class leaves the amounts alone (the share falls by itself);
//   - a multiply, a mix or a clamp does to the amounts what it does to the colour.
// Inside getLightColor the classes are kept PER CHANNEL, in light units (gMask*Light), because
// the clamps there are per channel and the texel multiplies per channel; getLightColor's return
// turns them into amounts of the lit colour (LightMaskToSurface). From there on everything the
// colour goes through is a scalar multiply, a mix or an addition, so plain amounts are exact.
//
// Pinned light today is BEAM light: BeamLightAt on the surface, the mist it lights (FogSlabAt)
// and BeamAirGlow. Emissive sources (glow lanes, sweeps, stamps, the outline, glow textures,
// brightmaps, shapes, ignite) join in E4a; their plumbing is already here, so they only add.
//
layout(location = LIGHT_MASK_LOCATION) out vec4 FragLightMask;
vec3 gMaskEmissiveLight = vec3(0.0);
vec3 gMaskPinnedLight = vec3(0.0);
float gMaskEmissive = 0.0;
float gMaskPinned = 0.0;
// FogSlabAt's beam-lit mist, per channel, as it adds it to the mist's colour.
vec3 gFogSlabPinnedLight = vec3(0.0);

// A scalar multiply of the fragment colour.
void LightMaskScale(float factor)
{
	gMaskEmissive *= factor;
	gMaskPinned *= factor;
}

// A per-channel multiply of the light inside getLightColor.
void LightMaskScaleLight(vec3 factor)
{
	gMaskEmissiveLight *= factor;
	gMaskPinnedLight *= factor;
}

// A light operation this shader cannot see into (a custom ProcessLight): each channel keeps its
// share. The default ProcessLight returns its input, and x / x is exactly 1.
void LightMaskRescaleLight(vec3 before, vec3 after)
{
	vec3 factor = vec3(1.0);
	if (before.r > 1.0e-6) factor.r = max(after.r / before.r, 0.0);
	if (before.g > 1.0e-6) factor.g = max(after.g / before.g, 0.0);
	if (before.b > 1.0e-6) factor.b = max(after.b / before.b, 0.0);
	LightMaskScaleLight(factor);
}

// getLightColor's return: the classes' light as amounts of the lit colour. ProcessMaterialLight
// multiplies the light by the texel per channel (material_normal.fp: Base x (light + dynamic
// lights)), so a class's part of the lit colour is Base . classLight -- exact while no clamp
// bites. Its 1.4 clamp, the colour-correcting light mode and subtractive lights can leave less
// light than that; the classes together never claim more than the pixel has.
void LightMaskToSurface(vec3 surfaceBase, vec3 litColour)
{
	gMaskEmissive = max(dot(surfaceBase, gMaskEmissiveLight), 0.0);
	gMaskPinned = max(dot(surfaceBase, gMaskPinnedLight), 0.0);
	float litTotal = max(litColour.r + litColour.g + litColour.b, 0.0);
	float classTotal = gMaskEmissive + gMaskPinned;
	if (classTotal > litTotal)
		LightMaskScale(classTotal > 0.0 ? litTotal / classTotal : 0.0);
}
#endif
#ifdef MODEL_EYE_FADE
#ifndef uEyeFadeNear
// [EYEFADE] The Vulkan prolog maps uEyeFadeNear/uEyeFadeFar into the per-draw data. GL and
// GLES carry no such values and never select this effect: plain uniforms, never set, read
// 0, and the fade below stays off.
uniform float uEyeFadeNear;
uniform float uEyeFadeFar;
#endif
#endif

struct Material
{
	vec4 Base;
	vec4 Bright;
	vec4 Glow;
	vec3 Normal;
	vec3 Specular;
	float Glossiness;
	float SpecularLevel;
	float Metallic;
	float Roughness;
	float AO;
};

vec4 Process(vec4 color);
vec4 ProcessTexel();
Material ProcessMaterial(); // note that this is deprecated. Use SetupMaterial!
void SetupMaterial(inout Material mat);
vec4 ProcessLight(Material mat, vec4 color);
vec3 ProcessMaterialLight(Material material, vec3 color);
vec2 GetTexCoord();

// These get Or'ed into uTextureMode because it only uses its 3 lowermost bits.
const int TEXF_Brightmap = 0x10000;
const int TEXF_Detailmap = 0x20000;
const int TEXF_Glowmap = 0x40000;
const int TEXF_ClampY = 0x80000;
const int TEXF_FlipNormal = 0x100000;

//===========================================================================
//
// RGB to HSV
//
//===========================================================================

vec3 rgb2hsv(vec3 c)
{
	vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

//===========================================================================
//
// Color to grayscale
//
//===========================================================================

float grayscale(vec4 color)
{
	return dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 dodesaturate(vec4 texel, float factor)
{
#ifdef SHADER_LITE
	return texel;
#else
	if (factor != 0.0)
	{
		float gray = grayscale(texel);

		// [BB] WHAT SURVIVES THE DRAIN.
		//
		// Desaturation was all or nothing, so a monochrome preset made blood
		// exactly as grey as the wall it was sprayed on. Weighting the drain by
		// a colour's OWN saturation means a world can be grey and still keep
		// the vivid things in it -- blood, a keycard, a kill badge -- without
		// one actor, sprite or texture being tagged.
		//
		// THIS FUNCTION IS THE ONLY DESATURATION IN THE SHADER. Textures,
		// sprites, glow, sweep bands, brightmaps and the flat-edge glow all
		// route through it, so the rule lands on every one of them and cannot
		// disagree with itself.
		//
		//   uDesatKeep  x threshold, y softness, z hue gate, w spare
		//
		// Threshold 0 skips the whole block and the result is bit-for-bit what
		// it was before.
		if (uDesatKeep.x > 0.0)
		{
			// Chroma as a fraction of brightness -- HSV saturation. Cheap, and
			// it is the quantity the eye actually reads as "how colourful",
			// which a channel difference alone is not.
			float mx = max(max(texel.r, texel.g), texel.b);
			float mn = min(min(texel.r, texel.g), texel.b);
			float sat = (mx > 0.0001) ? (mx - mn) / mx : 0.0;

			// HUE GATE, by dominant channel. Crude next to a real hue angle
			// and it needs no atan: blood is red-dominant, nukage is
			// green-dominant, and that is the whole question being asked.
			// Without it every saturated thing survives, which is a different
			// and also useful look -- so 0 means "any hue".
			int gate = int(uDesatKeep.z);
			bool hueOk = (gate == 0)
				|| (gate == 1 && texel.r >= mx)
				|| (gate == 2 && texel.g >= mx)
				|| (gate == 3 && texel.b >= mx);

			if (hueOk)
			{
				float lo = uDesatKeep.x;
				float hi = min(lo + max(uDesatKeep.y, 0.001), 1.0);
				factor *= 1.0 - smoothstep(lo, hi, sat);
			}
		}

		return mix (texel, vec4(gray,gray,gray,texel.a), factor);
	}
	else
	{
		return texel;
	}
#endif
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 desaturate(vec4 texel)
{
	// [BB] TWO SOURCES, AND THE STRONGER ONE WINS.
	//
	// uDesaturationFactor is the sector's own colormap byte, per draw.
	// uDesatKeep.w is a scene-global drain a mod can set with one call --
	// which is the difference between "grey this map" costing one number and
	// costing a walk over every sector in it.
	//
	// max() rather than add or replace: a sector the mapper deliberately
	// drained harder than the global stays drained harder, and turning the
	// global off restores it rather than flattening it.
	return dodesaturate(texel, max(uDesaturationFactor, uDesatKeep.w));
}

//===========================================================================
//
// Texture tinting code originally from JFDuke but with a few more options
//
//===========================================================================

const int Tex_Blend_Alpha = 1;
const int Tex_Blend_Screen = 2;
const int Tex_Blend_Overlay = 3;
const int Tex_Blend_Hardlight = 4;

 vec4 ApplyTextureManipulation(vec4 texel, int blendflags)
 {
	// Step 1: desaturate according to the material's desaturation factor.
	texel = dodesaturate(texel, uTextureModulateColor.a);

	// Step 2: Invert if requested
	if ((blendflags & 8) != 0)
	{
		texel.rgb = vec3(1.0 - texel.r, 1.0 - texel.g, 1.0 - texel.b);
	}

	// Step 3: Apply additive color
	texel.rgb += uTextureAddColor.rgb;

	// Step 4: Colorization, including gradient if set.
	texel.rgb *= uTextureModulateColor.rgb;

	// Before applying the blend the value needs to be clamped to [0..1] range.
	texel.rgb = clamp(texel.rgb, 0.0, 1.0);

	// Step 5: Apply a blend. This may just be a translucent overlay or one of the blend modes present in current Build engines.
	if ((blendflags & 7) != 0)
	{
		vec3 tcol = texel.rgb * 255.0;	// * 255.0 to make it easier to reuse the integer math.
		vec4 tint = uTextureBlendColor * 255.0;

		switch (blendflags & 7)
		{
			default:
				tcol.b = tcol.b * (1.0 - uTextureBlendColor.a) + tint.b * uTextureBlendColor.a;
				tcol.g = tcol.g * (1.0 - uTextureBlendColor.a) + tint.g * uTextureBlendColor.a;
				tcol.r = tcol.r * (1.0 - uTextureBlendColor.a) + tint.r * uTextureBlendColor.a;
				break;
			// The following 3 are taken 1:1 from the Build engine
			case Tex_Blend_Screen:
				tcol.b = 255.0 - (((255.0 - tcol.b) * (255.0 - tint.r)) / 256.0);
				tcol.g = 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 256.0);
				tcol.r = 255.0 - (((255.0 - tcol.r) * (255.0 - tint.b)) / 256.0);
				break;
			case Tex_Blend_Overlay:
				tcol.b = tcol.b < 128.0? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tcol.g < 128.0? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tcol.r < 128.0? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
			case Tex_Blend_Hardlight:
				tcol.b = tint.b < 128.0 ? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tint.g < 128.0 ? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tint.r < 128.0 ? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
		}
		texel.rgb = tcol / 255.0;
	}
	return texel;
}

//===========================================================================
//
// This function is common for all (non-special-effect) fragment shaders
//
//===========================================================================

vec4 getTexel(vec2 st)
{
	vec4 texel = texture(tex, st);

	//
	// Apply texture modes
	//
	switch (uTextureMode & 0xffff)
	{
		case 1:	// TM_STENCIL
			texel.rgb = vec3(1.0,1.0,1.0);
			break;

		case 2:	// TM_OPAQUE
			texel.a = 1.0;
			break;

		case 3:	// TM_INVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, texel.a);
			break;

		case 4:	// TM_ALPHATEXTURE
		{
			float gray = grayscale(texel);
			texel = vec4(1.0, 1.0, 1.0, gray*texel.a);
			break;
		}

		case 5:	// TM_CLAMPY
			if (st.t < 0.0 || st.t > 1.0)
			{
				texel.a = 0.0;
			}
			break;

		case 6: // TM_OPAQUEINVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, 1.0);
			break;

		case 7: //TM_FOGLAYER
			return texel;

	}
#ifndef SHADER_LITE
	if ((uTextureMode & TEXF_ClampY) != 0)
	{
		if (st.t < 0.0 || st.t > 1.0)
		{
			texel.a = 0.0;
		}
	}

	// Apply the texture modification colors.
	int blendflags = int(uTextureAddColor.a);	// this alpha is unused otherwise
	if (blendflags != 0)
	{
		// only apply the texture manipulation if it contains something.
		texel = ApplyTextureManipulation(texel, blendflags);
	}

	// Apply the Doom64 style material colors on top of everything from the texture modification settings.
	// This may be a bit redundant in terms of features but the data comes from different sources so this is unavoidable.
	texel.rgb += uAddColor.rgb;
	if (uObjectColor2.a == 0.0) texel *= uObjectColor;
	else texel *= mix(uObjectColor, uObjectColor2, gradientdist.z);
#else
	texel *= uObjectColor;
#endif
	// Last but not least apply the desaturation from the sector's light.
	return desaturate(texel);
}

//===========================================================================
//
// Vanilla Doom wall colormap equation
//
//===========================================================================
float R_WallColormap(float lightnum, float z, vec3 normal)
{
	// R_ScaleFromGlobalAngle calculation
	float projection = 160.0; // projection depends on SCREENBLOCKS!! 160 is the fullscreen value
	vec2 line_v1 = pixelpos.xz; // in vanilla this is the first curline vertex
	vec2 line_normal = normal.xz;
	float texscale = projection * clamp(dot(normalize(uCameraPos.xz - line_v1), line_normal), 0.0, 1.0) / z;

	float lightz = clamp(16.0 * texscale, 0.0, 47.0);

	// scalelight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	return startmap - lightz * 0.5;
}

//===========================================================================
//
// Vanilla Doom plane colormap equation
//
//===========================================================================
float R_PlaneColormap(float lightnum, float z)
{
	float lightz = clamp(z / 16.0f, 0.0, 127.0);

	// zlight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	float scale = 160.0 / (lightz + 1.0);
	return startmap - scale * 0.5;
}

//===========================================================================
//
// zdoom colormap equation
//
//===========================================================================
float R_ZDoomColormap(float light, float z)
{
	float L = light * 255.0;
	float vis = min(uGlobVis / z, 24.0 / 32.0);
	float shade = 2.0 - (L + 12.0) / 128.0;
	float lightscale = shade - vis;
	return lightscale * 31.0;
}

float R_DoomColormap(float light, float z)
{
#ifdef SHADER_LITE
	return R_ZDoomColormap(light, z);
#else
	if ((uPalLightLevels >> 16) == 16) // gl_lightmode 16
	{
		float lightnum = clamp(light * 15.0, 0.0, 15.0);

		if (dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5)
		{
			vec3 normal = normalize(vWorldNormal.xyz);
			return mix(R_WallColormap(lightnum, z, normal), R_PlaneColormap(lightnum, z), abs(normal.y));
		}
		else // vWorldNormal is not set on sprites
		{
			return R_PlaneColormap(lightnum, z);
		}
	}
	else
	{
		return R_ZDoomColormap(light, z);
	}
#endif	
}

//===========================================================================
//
// Doom software lighting equation
//
//===========================================================================
float R_DoomLightingEquation(float light)
{
	// z is the depth in view space, positive going into the screen
	float z;
	if (((uPalLightLevels >> 8)  & 0xff) == 2)
	{
		z = distance(pixelpos.xyz, uCameraPos.xyz);
	}
	else
	{
		z = pixelpos.w;
	}
#ifndef SHADER_LITE
	if ((uPalLightLevels >> 16) == 5) // gl_lightmode 5: Build software lighting emulation.
	{
		// This is a lot more primitive than Doom's lighting...
		float numShades = float(uPalLightLevels & 255);
		float curshade = (1.0 - light) * (numShades - 1.0);
		float visibility = max(uGlobVis * uLightFactor * z, 0.0);
		float shade = clamp((curshade + visibility), 0.0, numShades - 1.0);
		return clamp(shade * uLightDist, 0.0, 1.0);
	}
#endif
	float colormap = R_DoomColormap(light, z);

	if ((uPalLightLevels & 0xff) != 0)
		colormap = floor(colormap) + 0.5;

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(colormap, 0.0, 31.0) / 32.0;
}

//===========================================================================
//
// Check if light is in shadow
//
//===========================================================================

#ifdef SUPPORTS_RAYTRACING

bool traceHit(vec3 origin, vec3 direction, float dist)
{
	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TopLevelAS, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.01f, direction, dist);
	while(rayQueryProceedEXT(rayQuery)) { }
	return rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

vec2 softshadow[9 * 3] = vec2[](
	vec2( 0.0, 0.0),
	vec2(-2.0,-2.0),
	vec2( 2.0, 2.0),
	vec2( 2.0,-2.0),
	vec2(-2.0, 2.0),
	vec2(-1.0,-1.0),
	vec2( 1.0, 1.0),
	vec2( 1.0,-1.0),
	vec2(-1.0, 1.0),

	vec2( 0.0, 0.0),
	vec2(-1.5,-1.5),
	vec2( 1.5, 1.5),
	vec2( 1.5,-1.5),
	vec2(-1.5, 1.5),
	vec2(-0.5,-0.5),
	vec2( 0.5, 0.5),
	vec2( 0.5,-0.5),
	vec2(-0.5, 0.5),

	vec2( 0.0, 0.0),
	vec2(-1.25,-1.75),
	vec2( 1.75, 1.25),
	vec2( 1.25,-1.75),
	vec2(-1.75, 1.75),
	vec2(-0.75,-0.25),
	vec2( 0.25, 0.75),
	vec2( 0.75,-0.25),
	vec2(-0.25, 0.75)
);

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	if (shadowIndex >= 1024.0)
		return 1.0; // Don't cast rays for this light

	vec3 origin = pixelpos.xzy;
	vec3 target = lightpos.xzy + 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	vec3 direction = normalize(target - origin);
	float dist = distance(origin, target);

	if (uShadowmapFilter <= 0)
	{
		return traceHit(origin, direction, dist) ? 0.0 : 1.0;
	}
	else
	{
		vec3 v = (abs(direction.x) > abs(direction.y)) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 xdir = normalize(cross(direction, v));
		vec3 ydir = cross(direction, xdir);

		// [RTSHADOWFIX] ("Engine docs/EFFECT_LIGHTS_LB_IMPL_NOTES.md") The loop casts step_count + 1 rays, so the sum is divided
		// by that many: dividing by step_count made a fully lit pixel 10/9 bright. softshadow holds 27 offsets and filter N reads
		// up to index 9N, so the ray-traced filter stops at 2 (19 rays): 3 and up read past the array.
		float sum = 0.0;
		int step_count = min(uShadowmapFilter, 2) * 9;
		for (int i = 0; i <= step_count; i++)
		{
			vec3 pos = target + xdir * softshadow[i].x + ydir * softshadow[i].y;
			sum += traceHit(origin, normalize(pos - origin), dist) ? 0.0 : 1.0;
		}
		return sum / float(step_count + 1);
	}
}

#else
#ifdef SUPPORTS_SHADOWMAPS

float shadowDirToU(vec2 dir)
{
	if (abs(dir.y) > abs(dir.x))
	{
		float x = dir.x / dir.y * 0.125;
		if (dir.y >= 0.0)
			return 0.125 + x;
		else
			return (0.50 + 0.125) + x;
	}
	else
	{
		float y = dir.y / dir.x * 0.125;
		if (dir.x >= 0.0)
			return (0.25 + 0.125) - y;
		else
			return (0.75 + 0.125) - y;
	}
}

vec2 shadowUToDir(float u)
{
	u *= 4.0;
	vec2 raydir;
	switch (int(u))
	{
	case 0: raydir = vec2(u * 2.0 - 1.0, 1.0); break;
	case 1: raydir = vec2(1.0, 1.0 - (u - 1.0) * 2.0); break;
	case 2: raydir = vec2(1.0 - (u - 2.0) * 2.0, -1.0); break;
	case 3: raydir = vec2(-1.0, (u - 3.0) * 2.0 - 1.0); break;
	}
	return raydir;
}

float sampleShadowmap(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	ivec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x) * 0.25;

	// Snap to shadow map texel grid
	if (abs(ray.z) > abs(ray.x))
	{
		ray.y = ray.y / abs(ray.z);
		ray.x = ray.x / abs(ray.z);
		ray.x = (floor((ray.x + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.z = sign(ray.z);
	}
	else
	{
		ray.y = ray.y / abs(ray.x);
		ray.z = ray.z / abs(ray.x);
		ray.z = (floor((ray.z + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.x = sign(ray.x);
	}

	float t = negD / dot(vWorldNormal.xyz, ray) - bias;
	vec2 dir = ray.xz * t;

	float u = shadowDirToU(dir);
	float dist2 = dot(dir, dir);
	return step(dist2, texture(ShadowMap, vec2(u, v)).x);
}

float sampleShadowmapPCF(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	if (abs(ray.z) > abs(ray.x))
		ray.y = ray.y / abs(ray.z);
	else
		ray.y = ray.y / abs(ray.x);

	ivec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x);
	float texelPos = floor(shadowDirToU(ray.xz) * scale);

	float sum = 0.0;
	float step_count = float(uShadowmapFilter);

	texelPos -= step_count + 0.5;
	for (float x = -step_count; x <= step_count; x++)
	{
		float u = fract(texelPos / scale);
		vec2 dir = shadowUToDir(u);

		ray.x = dir.x;
		ray.z = dir.y;
		float t = negD / dot(vWorldNormal.xyz, ray) - bias;
		dir = ray.xz * t;

		float dist2 = dot(dir, dir);
		sum += step(dist2, texture(ShadowMap, vec2(u, v)).x);
		texelPos++;
	}
	return sum / (float(uShadowmapFilter) * 2.0 + 1.0);
}

float shadowmapAttenuation(vec4 lightpos, float shadowIndex)
{
	if (shadowIndex >= 1024.0)
		return 1.0; // No shadowmap available for this light

	vec3 planePoint = pixelpos.xyz - lightpos.xyz;
	planePoint += 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	if (dot(planePoint.xz, planePoint.xz) < 1.0)
		return 1.0; // Light is too close

	float v = (shadowIndex + 0.5) / 1024.0;

	if (uShadowmapFilter <= 0)
	{
		return sampleShadowmap(planePoint, v);
	}
	else
	{
		return sampleShadowmapPCF(planePoint, v);
	}
}

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	return shadowmapAttenuation(lightpos, shadowIndex);
}

#else

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	return 1.0;
}

#endif
#endif

float spotLightAttenuation(vec4 lightpos, vec3 spotdir, float lightCosInnerAngle, float lightCosOuterAngle)
{
	vec3 lightDirection = normalize(lightpos.xyz - pixelpos.xyz);
	float cosDir = dot(lightDirection, spotdir);
	return smoothstep(lightCosOuterAngle, lightCosInnerAngle, cosDir);
}

#ifdef EFFECT_LIGHTS

//===========================================================================
//
// [EFFECTLIGHTS] EFFECT LIGHTS ON THIS PIXEL ("Engine docs/EFFECT_LIGHTS_CORE_IMPL_NOTES.md"; LIGHTS_20_21_22_PLAN.md 2b, 2d,
// 2g, 2h). Short lights that belong to no actor -- sparks, embers, impacts, tracers -- which the CPU sorts into world bins
// around the eye once a frame, for both eyes (hw_effectlights.cpp). A pixel loops only its own bin.
//
// A light is a segment a -> b with a radius, seen from the closest point on it: attenuation (r - d) / r and N.L toward that
// point. With a == b the closest point is a, and this is material_normal.fp's point light, statement for statement. A
// residual glow (a crowded bin's merged lights) does not face. A light with a shadow-map row is blocked by walls.
//
// The layouts are hw_effectlightbuffer.h's; set 1 bindings 14 and 15 are in every pipeline layout (vk_descriptorset.cpp). The
// per-draw gate uEffectLightMode is StreamData's padding2 slot, 0 on every draw that never sets it: 0 no effect light, 1 with
// N.L (walls, flats, models), 2 without (sprites, whose normal is zero).
//
//===========================================================================

struct EffectLightRecord
{
	vec4 a;			// xyz the start this frame        w radius
	vec4 b;			// xyz the end (== a: a point)     w flags
	vec4 color;		// rgb the light now               w shadow-map row, -1 none
	vec4 extra;		// xyz where the row was cast from w brightness at b as a share of a's
};

layout(set = 1, binding = 14, std430) buffer readonly EffectLightSSO
{
	uint effectLightCount;
	uint effectLightBinned;
	uint effectLightResiduals;
	uint effectLightSerial;
	EffectLightRecord effectLights[];
};

layout(set = 1, binding = 15, std430) buffer readonly EffectLightBinSSO
{
	vec4 effectLightGridCorner;		// xyz bin (0, 0, 0)'s lowest corner   w the bin size
	ivec4 effectLightGridSize;		// xyz bins a side (0: none binned)    w where the index list starts
	uint effectLightBinData[];		// (offset << 6) | count a bin, then the index list
};

#define uEffectLightMode data[uDataIndex].padding2

const int EFFECT_LIGHT_FACING = 1;
const int EFFECT_LIGHT_ROW = 2;
const int EFFECT_LIGHT_NOSURFACES = 8;
const int EFFECT_LIGHT_BIN_LOOP_MAX = 32;

// Walls between this pixel and where the light's row was cast from: one tap of the shadow map (one ray when ray traced).
float EffectLightWallAttenuation(vec3 origin, float row, int mode)
{
#ifdef SUPPORTS_RAYTRACING
	vec3 rayStart = pixelpos.xzy;
	vec3 target = origin.xzy + 0.01;
	return traceHit(rayStart, normalize(target - rayStart), distance(rayStart, target)) ? 0.0 : 1.0;
#else
#ifdef SUPPORTS_SHADOWMAPS
	if (row < 0.0 || row >= 1024.0)
		return 1.0;
	vec3 planePoint = pixelpos.xyz - origin;
	planePoint += 0.01;
	if (dot(planePoint.xz, planePoint.xz) < 1.0)
		return 1.0;
	float v = (row + 0.5) / 1024.0;
	if (mode == 1)
		return sampleShadowmap(planePoint, v);
	// A sprite has no plane to snap the ray to: the distance along the texel's own direction.
	return step(dot(planePoint.xz, planePoint.xz), texture(ShadowMap, vec2(shadowDirToU(planePoint.xz), v)).x);
#else
	return 1.0;
#endif
#endif
}

vec3 EffectLightOnSurface(int index, vec3 p, vec3 normal, int mode)
{
	EffectLightRecord light = effectLights[index];
	int flags = int(light.b.w);
	if ((flags & EFFECT_LIGHT_NOSURFACES) != 0)
		return vec3(0.0);

	// The closest point on the segment. A point light is its own closest point, so a point takes material_normal.fp's path.
	vec3 segment = light.b.xyz - light.a.xyz;
	float lengthSquared = dot(segment, segment);
	float share = lengthSquared > 0.0 ? clamp(dot(p - light.a.xyz, segment) / lengthSquared, 0.0, 1.0) : 0.0;
	vec3 closest = share > 0.0 ? light.a.xyz + segment * share : light.a.xyz;

	float lightdistance = distance(closest, p);
	if (light.a.w < lightdistance)
		return vec3(0.0);

	float attenuation = clamp((light.a.w - lightdistance) / light.a.w, 0.0, 1.0);
	if (mode == 1 && (flags & EFFECT_LIGHT_FACING) != 0)
	{
		vec3 lightdir = normalize(closest - p);
		float dotprod = dot(normal, lightdir);
		if (dotprod < -0.0001)
			return vec3(0.0);
		attenuation *= clamp(dotprod, 0.0, 1.0);
	}
	if (share > 0.0)
		attenuation *= mix(1.0, light.extra.w, share);
	if (attenuation <= 0.0)
		return vec3(0.0);
	if ((flags & EFFECT_LIGHT_ROW) != 0)
		attenuation *= EffectLightWallAttenuation(light.extra.xyz, light.color.w, mode);
	return light.color.rgb * attenuation;
}

// The effect light at p from the lights of p's bin.
vec3 EffectLightsAt(vec3 p, vec3 normal, int mode)
{
	if (effectLightGridSize.x <= 0)
		return vec3(0.0);
	vec3 cellf = floor((p - effectLightGridCorner.xyz) / effectLightGridCorner.w);
	if (any(lessThan(cellf, vec3(0.0))) || any(greaterThanEqual(cellf, vec3(effectLightGridSize.xyz))))
		return vec3(0.0);
	ivec3 cell = ivec3(cellf);
	uint word = effectLightBinData[(cell.z * effectLightGridSize.y + cell.y) * effectLightGridSize.x + cell.x];
	int first = effectLightGridSize.w + int(word >> 6u);
	int count = min(int(word & 63u), EFFECT_LIGHT_BIN_LOOP_MAX);
	int records = int(effectLightCount);
	vec3 total = vec3(0.0);
	for (int k = 0; k < count; k++)
	{
		int index = int(effectLightBinData[first + k]);
		if (index < records)
			total += EffectLightOnSurface(index, p, normal, mode);
	}
	return total;
}

#endif

//===========================================================================
//
// Adjust normal vector according to the normal map
//
//===========================================================================

#if defined(NORMALMAP)
mat3 cotangent_frame(vec3 n, vec3 p, vec2 uv)
{
	// get edge vectors of the pixel triangle
	vec3 dp1 = dFdx(p);
	vec3 dp2 = dFdy(p);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);

	// solve the linear system
	vec3 dp2perp = cross(n, dp2); // cross(dp2, n);
	vec3 dp1perp = cross(dp1, n); // cross(n, dp1);
	vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame
	float invmax = inversesqrt(max(dot(t,t), dot(b,b)));
	return mat3(t * invmax, b * invmax, n);
}

vec3 ApplyNormalMap(vec2 texcoord)
{
	#define WITH_NORMALMAP_UNSIGNED
	#define WITH_NORMALMAP_GREEN_UP
	//#define WITH_NORMALMAP_2CHANNEL

	vec3 interpolatedNormal = normalize(vWorldNormal.xyz);

	vec3 map = texture(normaltexture, texcoord).xyz;
	#if defined(WITH_NORMALMAP_UNSIGNED)
	map = map * 255./127. - 128./127.; // Math so "odd" because 0.5 cannot be precisely described in an unsigned format
	#endif
	#if defined(WITH_NORMALMAP_2CHANNEL)
	map.z = sqrt(1 - dot(map.xy, map.xy));
	#endif
	#if defined(WITH_NORMALMAP_GREEN_UP)
	map.y = -map.y;
	#endif

	mat3 tbn = cotangent_frame(interpolatedNormal, pixelpos.xyz, vTexCoord.st);
	vec3 bumpedNormal = normalize(tbn * map);
	if ((uTextureMode & TEXF_FlipNormal) != 0)
	{
		bumpedNormal = -bumpedNormal;
	}
	return bumpedNormal;
}
#else
vec3 ApplyNormalMap(vec2 texcoord)
{
	vec3 normal = normalize(vWorldNormal.xyz);
	if ((uTextureMode & TEXF_FlipNormal) != 0)
	{
		normal = -normal;
	}
	return normal;
}
#endif

//===========================================================================
//
// Sets the common material properties.
//
//===========================================================================

void SetMaterialProps(inout Material material, vec2 texCoord)
{
#ifdef NPOT_EMULATION
	if (uNpotEmulation.y != 0.0)
	{
		float period = floor(texCoord.t / uNpotEmulation.y);
		texCoord.s += uNpotEmulation.x * floor(mod(texCoord.t, uNpotEmulation.y));
		texCoord.t = period + mod(texCoord.t, uNpotEmulation.y);
	}
#endif
	material.Base = getTexel(texCoord.st);
	material.Normal = ApplyNormalMap(texCoord.st);

// OpenGL doesn't care, but Vulkan pukes all over the place if these texture samplings are included in no-texture shaders, even though never called.
#ifndef NO_LAYERS
	if ((uTextureMode & TEXF_Brightmap) != 0)
		material.Bright = desaturate(texture(brighttexture, texCoord.st));

	if ((uTextureMode & TEXF_Detailmap) != 0)
	{
		vec4 Detail = texture(detailtexture, texCoord.st * uDetailParms.xy) * uDetailParms.z;
		material.Base.rgb *= Detail.rgb;
	}

	if ((uTextureMode & TEXF_Glowmap) != 0)
		material.Glow = desaturate(texture(glowtexture, texCoord.st));
#endif
}

// [RS fork] ONE DISTANCE FUNCTION FOR EVERY SHAPE ID.
//
// The band light, the fog bow and the glow wave each had their own copy of
// this chain, and they drifted: the bow measured shell (4) and the signed
// crossings (6-9) as rings, and the wave did not know 6-9 at all. Two systems
// that measure the world differently cannot be lined up, so they now share
// this one. The setters clamp ids to 0..9, so "anything else" never arrives;
// 0 is off and callers test it before calling.
//
//   1 ring (xz)     2 bar along X (abs)    3 bar along Y (abs)    4 sphere
//   5 rise (signed Y)   6 +X   7 +Y   8 -X   9 -Y   (6-9 signed)
//
// p and o are in shader space: Doom's Z is .y.
float SweepShapeDist(int shape, vec3 p, vec3 o)
{
	if (shape == 1)      return length(p.xz - o.xz);
	else if (shape == 2) return abs(p.x - o.x);
	else if (shape == 3) return abs(p.z - o.z);
	else if (shape == 5) return p.y - o.y;
	else if (shape == 6) return p.x - o.x;
	else if (shape == 7) return p.z - o.z;
	else if (shape == 8) return o.x - p.x;
	else if (shape == 9) return o.z - p.z;
	return length(p - o);   // 4, the sphere
}

// [round2 A2] WHICH AXIS A PLANE SHAPE IS PERPENDICULAR TO, AND WHICH WAY IT
// TRAVELS -- one answer shared by the air lattice and the surface fill.
//
// The air lattice used to know only 2, 3 and 5 and mirrored 5 onto the eye's
// side, while the surface fill picked its tangents from its own list. So Purge
// (6) and Curtain (7) painted a band on the walls and drew nothing in the air,
// and a rising sheet seen from below hung at origin - radius. Both paths now ask
// this, so the painted and air patterns cannot drift apart again.
//
//   axis   0 = x for 2/6/8,  2 = z for 3/7/9,  1 = y for 5,  -1 = not a plane
//   away   +1 for 5/6/7, -1 for 8/9, 0 for the unsigned bars 2/3, whose band
//          sits on BOTH sides of the origin -- the caller picks the eye's side.
//
// Shader space, so Doom's Z is .y, the same swizzle SweepShapeDist uses.
void SweepPlaneAxis(int shape, out int axis, out float away)
{
	axis = -1;
	away = 0.0;
	if (shape == 2 || shape == 6 || shape == 8)      axis = 0;
	else if (shape == 3 || shape == 7 || shape == 9) axis = 2;
	else if (shape == 5)                             axis = 1;
	if (shape == 5 || shape == 6 || shape == 7)      away = 1.0;
	else if (shape == 8 || shape == 9)               away = -1.0;
}

// [round2 C2] THE (ARC, HEIGHT) FRAME OF A RING OR SHELL, shared by the surface
// fill and the air lattice (C5), so the painted and air patterns cannot drift.
//
// uv.x is arc length at p's own distance from the vertical axis, uv.y height.
// Arc length rather than raw angle, so a line keeps its world width as the band
// grows. Shader space: Doom's Z is .y.
//
// Two things drew a seam along the -X ray, where atan wraps from +pi to -pi:
//   - the arc coordinate jumps by 2*pi*r there, so fwidth() of it was about
//     2*pi*r and the antialias smeared one solid line out from the origin.
//     aaU is how far the pixel moves in the world instead,
//     length(dFdx(rel)) + length(dFdy(rel)), which has no jump. (The spec's
//     length(vec2(dFdx(rel), dFdy(rel))) is not valid GLSL: two vec2s.)
//   - a spacing that does not divide the circumference left a part line at
//     the wrap. The U spacing is snapped so a WHOLE number of lines goes round
//     at the band's radius. The lines are then radial, meet across the wrap at
//     every distance, and wrapCount lets per-line hashes (jitter, flicker,
//     majors) agree across it too.
//
// Costs: the snap moves lines slightly, and while the band's radius changes the
// whole-line count steps by one every spacing / 2pi units, which re-spaces the
// lines by up to half a spacing on the -X side. A rotated pattern
// (uSweepFill2.x) mixes arc into height, so it still meets imperfectly at the wrap.
void SweepArcFrame(vec3 p, vec3 origin, float bandRadius, out vec2 uv, out float spacingU,
	out float wrapCount, out float aaU, out float aaV)
{
	vec2 rel = p.xz - origin.xz;
	float r = max(length(rel), 0.001);
	uv = vec2(atan(rel.y, rel.x) * r, p.y);

	spacingU = uSweepFill.x;
	wrapCount = 0.0;
	if (spacingU > 0.0)
	{
		wrapCount = max(floor(6.28318531 * max(bandRadius, 0.0) / spacingU + 0.5), 1.0);
		spacingU = 6.28318531 * r / wrapCount;
	}

	aaU = length(dFdx(rel)) + length(dFdy(rel));
	aaV = fwidth(p.y);
}

// [round2 C2] Turn a caller-supplied antialias width with the pattern's axes.
// aaU < 0 means "no supplied width" (SweepLineAxisAA takes fwidth itself), and
// is left alone. Otherwise each rotated axis gets the bound of the two it mixes.
void SweepRotateAA(float cs, float sn, inout float aaU, inout float aaV)
{
	if (aaU < 0.0) return;
	float u = abs(cs) * aaU + abs(sn) * aaV;
	aaV = abs(sn) * aaU + abs(cs) * aaV;
	aaU = u;
}

// [round2 C1] PICKET SPACING FOR A PLANE BAND, shared by the air pickets and
// the painted ones, so the bars on the wall are the bars in the air. (Fill 4
// used to exist only in the air, and drew as a grid on the walls behind.)
//
// The U spacing, SNAPPED TO A WHOLE NUMBER OF BARS across the published room
// along the plane's U axis: the z extent for 2/6/8, the x extent for 3/7/9 and
// 5. The spacing cvar stays the spacing you asked for; this only nudges it so
// the run divides the room exactly, which is the difference between bars that
// belong to the wall they end at and a pattern with a half-bar sliced off at
// the edge. With no room published it is the spacing as given.
float SweepPicketSpacing(int axis)
{
	float across = uSweepFill.x;
	if (uSweepRoomMax.w > 0.0)
	{
		float span;
		if (axis == 0) span = uSweepRoomMax.z - uSweepRoomMin.z;
		else           span = uSweepRoomMax.x - uSweepRoomMin.x;
		if (span > 1.0)
		{
			float n = max(floor(span / max(across, 1.0) + 0.5), 1.0);
			across = span / n;
		}
	}
	return across;
}

// [round2 C5] Where a ray e + d*t is within distance r of the origin: [tIn, tOut].
// d need not be unit length. False when the ray never gets that close. A ray
// that does not move across the measure at all (a ring's xz measure seen
// straight up or down) is inside for all of t or for none of it.
bool SweepRayInsideRadius(vec3 e, vec3 d, float r, out float tIn, out float tOut)
{
	tIn = 0.0;
	tOut = 0.0;
	float c = dot(e, e) - r * r;
	float a = dot(d, d);
	if (a < 0.000001)
	{
		if (c > 0.0) return false;
		tIn = -1.0e10;
		tOut = 1.0e10;
		return true;
	}
	float b = dot(e, d);
	float disc = b * b - a * c;
	if (disc <= 0.0) return false;
	float s = sqrt(disc);
	tIn = (-b - s) / a;
	tOut = (-b + s) / a;
	return true;
}

// [round2 C5] WHERE A VIEW RAY CROSSES A RING OR SHELL BAND'S WALL.
//
// A plane band is a slab, crossed once. A ring band is a thick-walled cylinder
// about the vertical axis and a shell a thick-walled sphere, and a ray can cross
// the wall twice: on its way into the hollow and on its way out. The nearer
// stretch that lies in front of the eye and in front of the pixel is the one
// drawn; the far one only when the near one is behind the eye or behind the
// surface. The walls are the plane slab's faces: outer at radius + aheadW,
// inner at radius - behindW (a growing ring's ahead is outward).
//
// Writes the stretch into [t0, t1] and returns false when none is visible.
bool SweepShellCrossing(int shape, vec3 eye, vec3 dir, vec3 o, float radius,
	float aheadW, float behindW, float fragDist, out float t0, out float t1)
{
	t0 = 0.0;
	t1 = 0.0;
	vec3 e = eye - o;
	vec3 d = dir;
	if (shape == 1) { e.y = 0.0; d.y = 0.0; }   // ring: measured in xz only

	float outerIn, outerOut;
	if (!SweepRayInsideRadius(e, d, radius + aheadW, outerIn, outerOut)) return false;

	// Near stretch: entering the outer wall to entering the hollow (or to
	// leaving the outer wall when the ray misses the hollow). Far stretch:
	// leaving the hollow to leaving the outer wall.
	float nearEnd = outerOut;
	float farStart = outerOut;
	float innerR = radius - behindW;
	float innerIn, innerOut;
	if (innerR > 0.0 && SweepRayInsideRadius(e, d, innerR, innerIn, innerOut))
	{
		nearEnd = innerIn;
		farStart = innerOut;
	}

	float n0 = max(outerIn, 0.0);
	float n1 = min(nearEnd, fragDist);
	if (n1 > n0) { t0 = n0; t1 = n1; return true; }
	float f0 = max(farStart, 0.0);
	float f1 = min(outerOut, fragDist);
	if (f1 > f0) { t0 = f0; t1 = f1; return true; }
	return false;
}

float SweepBandAttenAt(int sb)
{
	vec4 sband = uSweepBands[sb];
	if (sband.w <= 0.0) return 0.0;

	vec4 sorg = uSweepBandOrigin[sb];
	int smode = int(sorg.w);
	if (smode <= 0) return 0.0;

	// [BB] SIGNED SHAPES -- an actual sweep.
	//
	// Modes 2 and 3 are abs(), which makes them TWO planes moving apart from
	// the origin. That is a split, not a sweep: the word means one front that
	// starts at one end of the level and travels to the other, and until these
	// were added the only signed shape in here was 5, which is vertical only.
	// A band could rise through a map and could not cross one.
	//
	// Signed, so the band is a single plane at origin + radius and everything
	// behind it has already been passed. Put the origin off the near edge and
	// give it the map's span as its reach and it crosses the whole level once.
	//
	//   6  along +X          7  along +Y
	//   8  along -X          9  along -Y
	//
	// The negative pair exists so a sweep can come from either side without the
	// caller having to move the origin to the far edge and invert its own reach,
	// which is arithmetic every caller would otherwise repeat.
	// [RS fork] The chain that lived here is SweepShapeDist, shared with the
	// fog bow and the glow wave so the three cannot disagree again.
	float sdist = SweepShapeDist(smode, pixelpos.xyz, sorg.xyz);

	float ssigned = sdist - sband.x;
	float thick = max(sband.y, 0.001);
	float strail = abs(uSweepTrail);
	float sbehind = (uSweepTrail >= 0.0) ? -ssigned : ssigned;
	float swidth = (strail > thick && sbehind > 0.0) ? strail : thick;

	float b = abs(ssigned);
	if (b >= swidth) return 0.0;
	return pow(1.0 - b / swidth, max(sband.z, 0.01));
}

// [round2 B2] HOW FAR BEHIND A SWEEP FRONT THIS PIXEL IS, 0..1.
//
// A band whose word carries the passed bit (+256, SetSweepBandPassed) grades
// everything its front has already crossed. SweepShapeDist < radius is exactly
// "behind the front" for every shape -- inside a ring or shell, on both inner
// sides of the 2/3 bars, behind the signed 5-9 -- so this one test makes an
// after-look that tracks the line per pixel instead of per sector.
//
// Ramps in over uSweepPassed.w map units behind the front; the strongest band
// wins. The caller gates on uSweepPassedColor.w, so this never runs when off.
float SweepPassedAt()
{
	float strongest = 0.0;
	float ramp = max(uSweepPassed.w, 1.0);
	for (int sb = 0; sb < 8; sb++)
	{
		if (sb >= uSweepCount) break;
		int bandword = int(uSweepBands[sb].w);
		if (((bandword >> 8) & 1) == 0) continue;
		vec4 sorg = uSweepBandOrigin[sb];
		int shape = int(sorg.w);
		if (shape <= 0) continue;
		float ahead = SweepShapeDist(shape, pixelpos.xyz, sorg.xyz) - uSweepBands[sb].x;
		strongest = max(strongest, clamp(-ahead / ramp, 0.0, 1.0));
	}
	return strongest;
}

//===========================================================================
//
// Calculate light
//
// It is important to note that the light color is not desaturated
// due to ZDoom's implementation weirdness. Everything that's added
// on top of it, e.g. dynamic lights and glows are, though, because
// the objects emitting these lights are also.
//
// This is making this a bit more complicated than it needs to
// because we can't just desaturate the final fragment color.
//
//===========================================================================

//
// [BB] THE GLOW WAVE -- the axis a glow never had.
//
// A glow varies per pixel going UP a wall and is one flat value going ALONG
// it, because reach arrives as a single number for the whole surface. So a
// wall faded beautifully top to bottom and had a dead straight top edge from
// one end of the room to the other. Recolour bands fixed that for COLOUR.
// Nothing fixed it for SHAPE.
//
// This returns a signed -1..+1 modulation for this fragment. Feed it a
// channel's phase and the blocks below apply it to REACH -- so the edge
// itself rises and falls -- and separately to brightness and to the near/far
// colour boundary. One wave, three terms, and they look nothing like each
// other: reach moves the shape, brightness moves the light, and colour moves
// where one lane's colour becomes the other's INSIDE a band whose shape never
// changes at all.
//
// Distance is measured with the SAME five shapes as the sweep, from the
// wave's own origin, so a wave running along the floor and a band crossing
// the room can be given one shape and made to arrive together. Two systems
// that measure the world differently can never be lined up; two that share a
// distance function line up by construction.
//
//   uGlowWave        x wavelength, y speed, z sharpness, w shape
//   uGlowWaveDepth   x reach, y brightness, z colour, w detune
//   uGlowWavePhase   x wall top, y wall bottom, z floor, w ceiling
//   uGlowWaveOrigin  xyz origin, w per-room seed scatter
//
// Wavelength 0 means off, and every glow falls through to exactly the
// arithmetic it did before this existed.
//
float GlowWaveRaw(float phase, float seedOff)
{
	if (uGlowWave.x <= 0.0) return 0.0;

	int wshape = int(uGlowWave.w);
	vec3 wo = uGlowWaveOrigin.xyz;
	// [RS fork] The sweep's own distance function, so a wave given 6-9 to
	// match a crossing band follows that band instead of turning into rings.
	// Identical to the old chain for 1-5.
	float d = SweepShapeDist(wshape, pixelpos.xyz, wo);

	float t = d / uGlowWave.x + timer * uGlowWave.y + phase + seedOff;
	float w = 0.5 + 0.5 * sin(t);

	// DETUNE. One sine is legible as machinery within about ten seconds,
	// because it repeats and the eye finds the period. A second sine at a
	// wavelength that does not divide the first never resolves, so the room
	// keeps almost-repeating and never quite does. That is the whole
	// difference between "animated" and "alive", and it costs one sin.
	if (uGlowWaveDepth.w > 0.0)
	{
		float w2 = 0.5 + 0.5 * sin(t * 0.6180339887 + 1.7);
		w = mix(w, w * w2 * 2.0, uGlowWaveDepth.w);
	}

	// Sharpness narrows the CREST and never the trough, because it is a pow
	// on a 0..1 value. At 1 it is a plain swell; high, it is a spike through
	// an otherwise flat lane -- weather against machinery.
	w = pow(clamp(w, 0.0, 1.0), max(uGlowWave.z, 0.001));

	return 2.0 * w - 1.0;
}

// PER-ROOM SCATTER. Without this the entire map undulates as one organism,
// which reads as a filter over the game rather than as lighting in it. The
// seed is taken from geometry that is already uploaded and already differs
// per sector -- the glow plane's height for a wall, the first linedef
// endpoint for a flat -- so every room gets its own moment for nothing.
float GlowWaveSeedOff(float src)
{
	if (uGlowWaveOrigin.w <= 0.0) return 0.0;
	return fract(sin(src * 12.9898) * 43758.5453) * 6.2831853 * uGlowWaveOrigin.w;
}

//
// [BB] TEXTURE INSIDE THE GLOW.
//
// The wave varies a glow's EDGE. That is the right answer while the edge is
// visible, and no answer at all once coverage saturates -- turn reach up far
// enough and the wall is a solid card of colour with a wave moving an edge
// that is no longer on screen. Everything below happens INSIDE the lit area
// instead, so a maxed-out lane has somewhere left to go.
//
// All of it is a multiplier on the glow's own contribution, so nothing here
// can move a band's shape and every term is off at 0.
//
//   uGlowTex   x noise amount, y noise scale, z drift, w contrast
//   uGlowTex2  x flow amount, y flow spacing, z flow speed, w flow sharpness
//   uGlowTex3  x cell amount, y cell scale, z cell speed, w cell edge width
//   uGlowTex4  x disturbance reach, y state pulse depth, z state level,
//              w pulse rate multiplier (1 = the rate the level implies)
//
// Sampled in WORLD space, not surface space. A wall and the floor it meets
// then agree about the pattern crossing the join, which is what makes it read
// as something the room is made of rather than as a decal applied per
// surface. It also costs no tangent frame.
//

// Forward declaration. GITDHash21 is defined further down beside the fog
// field that first needed it, and GLSL will not call a function it has not
// seen. Declaring it here rather than moving the definition keeps each hash
// next to the code it was written for -- same reason SweepLineAxis is
// forward-declared for the air lattice.
float GITDHash21(vec2 p);

// Cheap 3D value noise, the same shape as the fog's 2D one. Two octaves,
// because one reads as blobs and two reads as material.
float GITDHash31(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float GITDNoise3(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(
		mix(mix(GITDHash31(i + vec3(0,0,0)), GITDHash31(i + vec3(1,0,0)), f.x),
		    mix(GITDHash31(i + vec3(0,1,0)), GITDHash31(i + vec3(1,1,0)), f.x), f.y),
		mix(mix(GITDHash31(i + vec3(0,0,1)), GITDHash31(i + vec3(1,0,1)), f.x),
		    mix(GITDHash31(i + vec3(0,1,1)), GITDHash31(i + vec3(1,1,1)), f.x), f.y), f.z);
}

//
// [BB] SIGNED DISTANCE FIELDS -- shapes drawn onto surfaces.
//
// A distance function answers ONE question: how far is this point from the
// edge of the shape? Negative inside, zero on the edge, positive outside. That
// is the whole idea, and almost everything else in this file already works
// this way without saying so -- a beam asks its distance from a segment, the
// tornado from an axis, the fog from a plane.
//
// WHAT THE FUNCTIONS BUY THAT INLINING WOULD NOT.
//
// Each primitive is named and separate rather than folded into the one place
// that calls it, for two reasons that are worth the handful of extra lines.
//
// First, they COMPOSE. min() is union, max() is intersection, max(a, -b) is
// subtraction, and a smooth min melts two shapes into one. A ring is a disc
// through abs(). A square outline is a square through abs(). Every shape below
// past the first two is one of the first two with an operator applied, which
// is why the list is short and the vocabulary is not.
//
// Second, they TRANSCRIBE. When the playsim eventually needs to ask the same
// question the shader is asking -- is the player standing inside this shape --
// these are the functions that get mirrored into ZScript, and mirroring a
// named three-line function is a transcription rather than an excavation. One
// definition per shape is also what stops the drawn shape and the tested shape
// drifting apart, which is the bug this codebase has already shipped three
// times in other forms.
//
// The same functions serve a flat decal and a raymarched solid without
// changing: sdBox does not care whether it is being asked about a floor or
// about a ray. Nothing here marches yet.
//

float sdCircle(vec2 p, float r)
{
	return length(p) - r;
}

// Exact, including the outside corners -- the max(d,0) term is what makes a
// point diagonally off the corner measure to the CORNER rather than to the
// nearer edge's infinite line.
float sdBox(vec2 p, vec2 b)
{
	vec2 d = abs(p) - b;
	return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

// Three folds of a hexagon's symmetry, then one edge.
float sdHexagon(vec2 p, float r)
{
	const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
	p = abs(p);
	p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
	p -= vec2(clamp(p.x, -k.z * r, k.z * r), r);
	return length(p) * sign(p.y);
}

float sdTriangle(vec2 p, float r)
{
	const float k = 1.732050808;
	p.x = abs(p.x) - r;
	p.y = p.y + r / k;
	if (p.x + k * p.y > 0.0) p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
	p.x -= clamp(p.x, -2.0 * r, 0.0);
	return -length(p) * sign(p.y);
}

// A plus sign: the union of two boxes, which is one min().
float sdCross(vec2 p, vec2 b)
{
	return min(sdBox(p, b), sdBox(p, b.yx));
}

// ---- operators -----------------------------------------------------------
//
// An OUTLINE is any shape through abs(): the distance to its edge, rather than
// to its inside. That single trick turns every filled primitive above into a
// hollow one, which is why there is no sdRing and no sdSquareOutline.
float opOutline(float d, float w) { return abs(d) - w; }

float opUnion(float a, float b)  { return min(a, b); }
float opSub(float a, float b)    { return max(a, -b); }
float opInter(float a, float b)  { return max(a, b); }

// The one that has no mesh equivalent: two shapes that MELT together as they
// approach instead of intersecting with a crease.
float opSmoothUnion(float a, float b, float k)
{
	float h = clamp(0.5 + 0.5 * (b - a) / max(k, 0.0001), 0.0, 1.0);
	return mix(b, a, h) - k * h * (1.0 - h);
}

vec2 opRotate(vec2 p, float deg)
{
	float a = radians(deg);
	float c = cos(a), s = sin(a);
	return vec2(p.x * c - p.y * s, p.x * s + p.y * c);
}

//
// [BB] The shapes, drawn where a surface passes through them.
//
// Flat decals, not solids: each shape is evaluated in the plane of whatever
// surface the fragment belongs to, and faded by how far that fragment is from
// the shape's own height. That is what makes it lie ON the floor rather than
// hang in the air, and it costs one subtract instead of a march.
//
// ORIENTATION IS A FILTER ON THE SURFACE NORMAL, not a second code path. A
// floor shape wants upward faces and a wall shape wants vertical ones, and
// which two axes the pattern runs in follows from the same test -- so "on the
// floor" and "on the wall" are the same six lines with a different pair picked
// out of the position.
//
// THE SEAM IS A SUBTRACTION, which is the entire reason the operators above
// exist as operators. A shape splitting open down the middle is that shape
// minus a widening slab, and what shows through the gap is a second colour
// masked by the ORIGINAL shape -- so the reveal is bounded by the thing that
// split rather than bleeding past its edge.
//
vec3 ShapesAt(vec3 fragPos, vec3 nrm)
{
	vec3 sum = vec3(0.0);
	if (uShapeParams.x <= 0.0) return sum;

	float soft = max(uShapeParams.x, 0.01);
	float hfade = max(uShapeParams.y, 1.0);

	float up = nrm.y;                 // +1 on a floor, 0 on a wall, -1 ceiling
	bool isFlat = abs(up) > 0.7;

	// HOW MANY SLOTS ARE ACTUALLY IN USE, and this is what makes a large cap
	// affordable. The array is 128 long; the loop runs to the high-water mark
	// instead, so three shapes cost three iterations and not a hundred and
	// twenty-five wasted compares on every pixel of every frame.
	//
	// Raising MAX_SHAPES without this would have been the whole cost with none
	// of the benefit -- the loop is per fragment, so the cap is multiplied by
	// five million before it reaches the frame time.
	int nshapes = int(uShapeParams.w);
	if (nshapes <= 0) return sum;

	for (int i = 0; i < 128; i++)
	{
		if (i >= nshapes) break;

		float size = uShapeA[i].w;
		if (size <= 0.0) continue;

		// CHEAP REJECT FIRST, before the orientation test, the rotation or any
		// distance function. A shape covers a small part of a room and a pixel
		// is near almost none of them, so this squared-radius compare is what
		// the loop actually spends its time on -- and when a whole warp misses
		// the same shape, which is the usual case because neighbouring pixels
		// are in the same place, the rest of the body is genuinely skipped.
		//
		// Squared on both sides: no sqrt, and the reach is the largest the
		// shape can grow to including its softness and its glow.
		// The reach has to include the FORMATION, not just the shape: a ring
		// of eight reaches its orbit radius, a tile field reaches its extent.
		// Rejecting on the shape's own size alone would clip a pattern to a
		// small disc around its anchor and look like the far copies were
		// missing rather than culled.
		float rmode = uShapeD[i].x;
		float spread = (rmode >= 1.0) ? max(uShapeD[i].y, uShapeD[i].z) : 0.0;

		vec3 rel3 = fragPos - uShapeA[i].xyz;
		float reach = size + spread + soft + uShapeParams.z + 1.0;
		if (dot(rel3, rel3) > reach * reach) continue;

		int packed_kind = int(uShapeB[i].x);
		int kind = packed_kind & 15;
		if (kind <= 0) continue;

		int orient = packed_kind >> 4;
		if (orient == 3) continue;               // standing -- StandingShapesAt() owns these
		if (orient == 0 && !isFlat) continue;    // floors only
		if (orient == 1 && isFlat) continue;     // walls only

		vec3 c = uShapeA[i].xyz;

		// The two axes the pattern runs in, and the one it fades along --
		// picked from the surface rather than from the shape, so a single
		// shape crossing a step draws correctly on both levels.
		vec2 uv;
		float off;
		if (isFlat) { uv = fragPos.xz - c.xz; off = fragPos.y - c.y; }
		else        { uv = vec2(fragPos.x - c.x, fragPos.y - c.y); off = fragPos.z - c.z; }

		float fade = 1.0 - smoothstep(0.0, hfade, abs(off));
		if (fade <= 0.0) continue;

		uv = opRotate(uv, uShapeB[i].y);

		// ---- REPEAT: one slot, many copies -------------------------------
		//
		// Folding the COORDINATE rather than drawing the shape N times. Eight
		// copies and eight hundred cost the same, because what changes is
		// where the point thinks it is, not how many distance tests run.
		//
		// It is the trick the laser lattice and the tendril field already
		// use, and the reason it does not replace the slots is that every
		// copy is necessarily identical -- same age, same colour, same fade.
		// A kill mark needs its own clock, so those stay one slot each.
		//
		// The anchor is the slot's own position, so a formation follows an
		// actor exactly as a single shape does. Dynamic and repeated are not
		// opposites.
		float patFade = 1.0;
		if (rmode >= 0.5 && rmode < 1.5)
		{
			// RADIAL. Fold the angle into one sector and every sector draws
			// the same shape -- N around a circle for the price of one.
			float cnt = max(floor(uShapeD[i].y), 1.0);
			float orbit = uShapeD[i].z;
			float ang = atan(uv.y, uv.x) + radians(uShapeD[i].w * timer);
			float sector = 6.2831853 / cnt;
			ang = mod(ang + sector * 0.5, sector) - sector * 0.5;
			uv = vec2(cos(ang), sin(ang)) * length(uv) - vec2(orbit, 0.0);
		}
		else if (rmode >= 1.5)
		{
			// GRID. mod() the plane into tiles. Infinite by nature, so it is
			// faded toward the stated extent rather than cut at it -- a hard
			// edge on a tiling field reads as the pattern being clipped by
			// something invisible.
			float ext = max(uShapeD[i].y, 1.0);
			float sp = max(uShapeD[i].z, 1.0);
			patFade = 1.0 - smoothstep(ext * 0.6, ext, length(uv));
			if (patFade <= 0.0) continue;
			uv += uShapeD[i].w * timer;
			uv = mod(uv + sp * 0.5, sp) - sp * 0.5;
		}

		float thick = max(uShapeB[i].z, 0.01);
		float d;
		if      (kind == 1) d = sdCircle(uv, size);
		else if (kind == 2) d = opOutline(sdCircle(uv, size), thick);
		else if (kind == 3) d = sdBox(uv, vec2(size));
		else if (kind == 4) d = opOutline(sdBox(uv, vec2(size)), thick);
		else if (kind == 5) d = sdCross(uv, vec2(size, thick));
		else if (kind == 6) d = sdHexagon(uv, size);
		else                d = sdTriangle(uv, size);

		float cov = 1.0 - smoothstep(0.0, soft, d);
		if (cov <= 0.0) continue;

		// SPLIT. A slab through the middle, widening with the seam value.
		// Taken out of the shape by subtraction and given back as the under
		// colour, masked by the shape it came out of.
		vec3 col = uShapeCol[i].rgb * uShapeCol[i].w;
		float seam = uShapeB[i].w;
		if (seam > 0.0)
		{
			float gap = abs(uv.x) - seam * size;
			float gcov = (1.0 - smoothstep(0.0, soft, gap)) * cov;
			cov -= gcov;
			sum += uShapeUnder.rgb * gcov * fade * patFade;
		}

		// A little reach past the edge, so a hard shape still sits in the room
		// rather than being stuck onto it.
		if (uShapeParams.z > 0.0)
			cov += (1.0 - smoothstep(0.0, uShapeParams.z, max(d, 0.0)))
			     * uShapeParams.z * 0.02;

		sum += col * max(cov, 0.0) * fade * patFade;
	}
	return sum;
}

//
// [BB] STANDING SHAPES -- the same shape library, freestanding in open air
// instead of painted on whatever surface happens to be nearby.
//
// ShapesAt() decals an EXISTING surface: it only ever runs where the
// G-buffer already has a normal, and it borrows that surface's own plane to
// work in. A standing shape has no surface to borrow -- it defines its OWN
// plane, an anchor point plus a full yaw/pitch/roll orientation (resolved
// natively, once a frame, from CPU-side g_levellocals.h -- see ShapePitch/
// ShapeRoll/the rate fields there, and hw_drawinfo.cpp's resolve loop for
// where rate * age and parent linking both apply before this ever runs),
// and asks where the EYE'S OWN VIEW RAY crosses that plane. That is the
// exact question BeamAirGlow already asks of a line segment; this asks it
// of a plane instead, and that ray-vs-plane solve is the only genuinely new
// maths in this whole feature. Every SDF, the seam split and both repeat
// modes below are the identical code ShapesAt() already runs, just fed a
// different uv -- reused, not reimplemented.
//
// ORIENT 3 IS THE ONLY ROW THIS FUNCTION EVER SEES. ShapesAt()'s own loop
// skips orient 3 (see the "standing" continue there), so nothing is ever
// evaluated by both functions.
//
// CALLED UNCONDITIONALLY, like BeamAirGlow and for the same reason: a
// standing shape has to be visible against open air or the sky, not only
// where the camera happens to already be looking at a floor or wall -- so it
// cannot live behind ShapesAt()'s "does this fragment have a world normal"
// gate.
//
vec3 StandingShapesAt(vec3 fragPos)
{
	vec3 sum = vec3(0.0);
	if (uShapeParams.x <= 0.0) return sum;

	float soft = max(uShapeParams.x, 0.01);

	int nshapes = int(uShapeParams.w);
	if (nshapes <= 0) return sum;

	vec3 eye = uCameraPos.xyz;
	vec3 toFrag = fragPos - eye;
	float fragDist = length(toFrag);
	if (fragDist < 0.001) return sum;
	vec3 dir = toFrag / fragDist;

	for (int i = 0; i < 128; i++)
	{
		if (i >= nshapes) break;

		float size = uShapeA[i].w;
		if (size <= 0.0) continue;

		int packed_kind = int(uShapeB[i].x);
		int kind = packed_kind & 15;
		if (kind <= 0) continue;

		int orient = packed_kind >> 4;
		if (orient != 3) continue;    // everything else is ShapesAt()'s

		vec3 c = uShapeA[i].xyz;

		// CHEAP REJECT, AGAINST THE RAY -- NOT AGAINST fragPos. fragPos is
		// wherever THIS PIXEL'S OWN surface is, which for a standing shape
		// can be a wall far behind it, or off to one side entirely; testing
		// distance to fragPos would reject shapes that are plainly on
		// screen. Testing the ray's own closest approach to the anchor is
		// what BeamAirGlow's cull step does against a segment's midpoint,
		// for the identical reason.
		float rmode = uShapeD[i].x;
		float spread = (rmode >= 1.0) ? max(uShapeD[i].y, uShapeD[i].z) : 0.0;
		float reach = size + spread + soft + uShapeParams.z + 1.0;

		vec3 ec = c - eye;
		float alongC = dot(ec, dir);
		vec3 perpC = ec - dir * alongC;
		if (dot(perpC, perpC) > reach * reach) continue;

		// THE PLANE. Full yaw/pitch/roll -- pitch tilts the facing normal
		// up or down, roll spins the panel around that normal once tilted.
		// Built from cross() rather than a hand-simplified closed form: the
		// cost is trivial next to the solve below, and a shader is a bad
		// place to debug a sign error in hand algebra.
		float ang  = radians(uShapeB[i].y);   // yaw
		float pit  = radians(uShapeE[i].x);   // pitch
		float rol  = radians(uShapeE[i].y);   // roll

		vec3 pnrm = vec3(cos(pit) * cos(ang), sin(pit), cos(pit) * sin(ang));
		vec3 worldUp = vec3(0.0, 1.0, 0.0);

		// DEGENERATE CASE: pitched to face straight up or down, where the
		// facing normal is parallel to world-up and cross() collapses to a
		// zero-length vector -- "right" would be undefined. Falls back to
		// world +X as the reference instead. Yaw-only shapes can never hit
		// this (pnrm.y is always exactly 0), which is why the earlier,
		// yaw-only version of this function never needed the guard.
		vec3 right0 = (abs(pnrm.y) > 0.999)
			? cross(vec3(1.0, 0.0, 0.0), pnrm)
			: cross(worldUp, pnrm);
		right0 = normalize(right0);
		vec3 up0 = cross(pnrm, right0);

		float cr = cos(rol), sr = sin(rol);
		vec3 right = right0 * cr + up0 * sr;
		vec3 up2   = up0 * cr - right0 * sr;

		// RAY VERSUS PLANE. Standard t = dot(N, planePoint - rayOrigin) /
		// dot(N, rayDir). A ray running parallel to the plane never crosses
		// it -- denom near zero -- and is skipped rather than divided by.
		float denom = dot(pnrm, dir);
		if (abs(denom) < 0.0001) continue;

		float t = dot(pnrm, c - eye) / denom;

		// BEHIND THE EYE, OR BEHIND REAL GEOMETRY. A crossing further away
		// than this pixel's own surface is occluded BY that surface. This is
		// an explicit branch rather than BeamAirGlow's clamp-the-solve trick,
		// on purpose: a plane has no segment to clamp the solve into, and
		// getting this one wrong the clever way means a standing shape
		// ghosts through the wall in front of it instead of just not
		// drawing -- the failure mode a depth test exists to prevent.
		if (t <= 0.0 || t >= fragDist) continue;

		vec3 hit = eye + dir * t;
		vec2 uv = vec2(dot(hit - c, right), dot(hit - c, up2));

		// ---- REPEAT: identical to ShapesAt(), same two modes -------------
		float patFade = 1.0;
		if (rmode >= 0.5 && rmode < 1.5)
		{
			float cnt = max(floor(uShapeD[i].y), 1.0);
			float orbit = uShapeD[i].z;
			float rang = atan(uv.y, uv.x) + radians(uShapeD[i].w * timer);
			float sector = 6.2831853 / cnt;
			rang = mod(rang + sector * 0.5, sector) - sector * 0.5;
			uv = vec2(cos(rang), sin(rang)) * length(uv) - vec2(orbit, 0.0);
		}
		else if (rmode >= 1.5)
		{
			float ext = max(uShapeD[i].y, 1.0);
			float sp = max(uShapeD[i].z, 1.0);
			patFade = 1.0 - smoothstep(ext * 0.6, ext, length(uv));
			if (patFade <= 0.0) continue;
			uv += uShapeD[i].w * timer;
			uv = mod(uv + sp * 0.5, sp) - sp * 0.5;
		}

		float thick = max(uShapeB[i].z, 0.01);
		float d;
		if      (kind == 1) d = sdCircle(uv, size);
		else if (kind == 2) d = opOutline(sdCircle(uv, size), thick);
		else if (kind == 3) d = sdBox(uv, vec2(size));
		else if (kind == 4) d = opOutline(sdBox(uv, vec2(size)), thick);
		else if (kind == 5) d = sdCross(uv, vec2(size, thick));
		else if (kind == 6) d = sdHexagon(uv, size);
		else                d = sdTriangle(uv, size);

		float cov = 1.0 - smoothstep(0.0, soft, d);
		if (cov <= 0.0) continue;

		// SPLIT: identical to ShapesAt() -- the seam is what a portal opens
		// along.
		vec3 col = uShapeCol[i].rgb * uShapeCol[i].w;
		float seam = uShapeB[i].w;
		if (seam > 0.0)
		{
			float gap = abs(uv.x) - seam * size;
			float gcov = (1.0 - smoothstep(0.0, soft, gap)) * cov;
			cov -= gcov;
			sum += uShapeUnder.rgb * gcov * patFade;
		}

		sum += col * max(cov, 0.0) * patFade;
	}
	return sum;
}

float GlowTextureAt(float seedOff)
{
	// Everything off is the common case and it must cost one compare.
	if (uGlowTex.x <= 0.0 && uGlowTex2.x <= 0.0 && uGlowTex3.x <= 0.0
	    && uGlowTex4.x <= 0.0 && uGlowTex4.y <= 0.0) return 1.0;

	vec3 p = pixelpos.xyz;
	float mul = 1.0;

	// ---- 1. NOISE, so the wash has body -------------------------------
	//
	// A lit wall is one flat brightness across its whole face. Real glowing
	// material is not: it is veined, uneven, brighter in patches. This is the
	// same move the fog's density field makes, and for the same reason -- a
	// uniform value is the single biggest tell that something is a filter
	// rather than a substance.
	if (uGlowTex.x > 0.0)
	{
		vec3 q = p * max(uGlowTex.y, 0.00001);
		q.y += timer * uGlowTex.z * 0.05;      // drifts, so it is not a decal
		float n = GITDNoise3(q) * 0.65 + GITDNoise3(q * 2.7 + 11.3) * 0.35;

		// Contrast pushes it from a gentle mottle toward hard patches of lit
		// and unlit, which is the difference between marble and plasma.
		n = clamp(0.5 + (n - 0.5) * max(uGlowTex.w, 0.001), 0.0, 1.0);
		mul *= mix(1.0, 0.25 + 1.5 * n, clamp(uGlowTex.x, 0.0, 1.0));
	}

	// ---- 2. FLOW, along the surface rather than across the room --------
	//
	// The wave arrives FROM an origin. This travels ALONG the surface, which
	// is a different axis of motion entirely and reads as current running
	// through the material rather than as weather passing over it.
	//
	// The axis is chosen from the normal so it means the same thing on every
	// surface: on a wall it runs vertically, on a floor or ceiling it runs
	// along world X. No tangent frame needed, and a wall and its floor still
	// agree because both are world-space quantities.
	if (uGlowTex2.x > 0.0)
	{
		float upness = abs(normalize(vWorldNormal.xyz + vec3(0.0, 0.0001, 0.0)).y);
		float axis = (upness > 0.7) ? p.x : p.y;

		float sp = max(uGlowTex2.y, 1.0);
		float t = axis / sp - timer * uGlowTex2.z + seedOff;
		float band = 0.5 + 0.5 * sin(t * 6.2831853);
		band = pow(band, max(uGlowTex2.w, 0.001));
		mul *= mix(1.0, 0.3 + 1.4 * band, clamp(uGlowTex2.x, 0.0, 1.0));
	}

	// ---- 3. CELLS, with light travelling their edges -------------------
	//
	// The lattice trick from the sweep, made organic. A hashed offset per
	// cell turns a grid into something irregular, and lighting the DISTANCE
	// TO THE NEAREST CELL EDGE rather than the cell itself gives veins
	// instead of tiles. Each cell pulses on its own clock, so the light
	// crawls rather than blinking together.
	//
	// Free at any density, exactly like the laser lattice -- this is a
	// pattern, not a set of objects.
	if (uGlowTex3.x > 0.0)
	{
		float cs = max(uGlowTex3.y, 1.0);
		vec3 cp = p / cs;
		vec3 base = floor(cp);

		// Nearest of the 3x3x3 neighbourhood would be 27 samples. Two axes
		// is enough for a surface and costs 9, and on a wall or a flat the
		// third axis is the one you cannot see anyway.
		float upness = abs(normalize(vWorldNormal.xyz + vec3(0.0, 0.0001, 0.0)).y);
		vec2 uv = (upness > 0.7) ? cp.xz : vec2(cp.x + cp.z, cp.y);
		vec2 cell = floor(uv);
		vec2 lf = fract(uv);

		float d1 = 8.0, d2 = 8.0;
		for (int gy = -1; gy <= 1; gy++)
		{
			for (int gx = -1; gx <= 1; gx++)
			{
				vec2 g = vec2(float(gx), float(gy));
				vec2 id = cell + g;
				float h = GITDHash21(id);
				float h2 = GITDHash21(id + 37.7);
				vec2 site = g + vec2(h, h2);
				float d = length(site - lf);
				// Keep the two nearest: their DIFFERENCE is the edge, which
				// is what turns cells into veins.
				if (d < d1) { d2 = d1; d1 = d; }
				else if (d < d2) { d2 = d; }
			}
		}

		float edge = d2 - d1;
		float w = max(uGlowTex3.w, 0.01);
		float vein = 1.0 - smoothstep(0.0, w, edge);

		// Each cell on its own clock, from its own hash, so the network
		// crawls instead of flashing as one.
		float ph = GITDHash21(cell + 91.7) * 6.2831853;
		vein *= 0.45 + 0.55 * (0.5 + 0.5 * sin(timer * uGlowTex3.z + ph));

		mul *= mix(1.0, 0.35 + 1.9 * vein, clamp(uGlowTex3.x, 0.0, 1.0));
	}

	// ---- 4. THE WALLS REACT TOO ---------------------------------------
	//
	// The disturbance array already exists, already fires on gunfire and on
	// death, and until now only the fog consumed it. Feeding the same slots
	// (32 now, MAX_FOG_DISTURB) into the glow costs nothing to build and makes a shot visibly
	// travel across the lit surfaces of the room rather than only through
	// the air in it.
	//
	// Rings only. A disc that dimmed the wall would read as damage to the
	// light rather than as a pulse through it.
	if (uGlowTex4.x > 0.0)
	{
		float pulse = 0.0;
		// STOP AT THE LIVE COUNT, the way ShapesAt() and the beam loops
		// already do. Without this the loop walks all 32 slots on every
		// fragment whether anything is live or not -- which cost 8 before
		// the cap was raised and 32 after, for the same usually-empty
		// array. uFogBow.w is the count the C++ side already uploads.
		int ndist = int(uFogBow.w);
		for (int di = 0; di < 32; di++)
		{
			if (di >= ndist) break;

			float stren = uFogDisturbB[di].y;
			if (stren <= 0.0) continue;

			float age = uFogDisturbB[di].x;
			float front = age * max(uFogDisturbB[di].z, 1.0);
			float r = distance(p, uFogDisturbA[di].xyz);
			float band = 1.0 - smoothstep(0.0, max(uFogDisturbA[di].w, 1.0), abs(r - front));
			pulse += band * stren;
		}
		mul *= 1.0 + clamp(pulse, 0.0, 4.0) * uGlowTex4.x;
	}

	// ---- 5. THE ROOM KNOWS SOMETHING IS WRONG --------------------------
	//
	// One level pushed from script -- monsters near you, your health, an
	// alarm -- driving a pulse through every glow at once. The lane stops
	// being decoration and starts carrying information, which is the thing
	// none of the four terms above can do no matter how good they look.
	if (uGlowTex4.y > 0.0)
	{
		// Rate rises with the level, so it is not just brighter when things
		// are bad -- it is FASTER, which is what reads as urgency.
		float lvl = clamp(uGlowTex4.z, 0.0, 1.0);
		// [RS fork] uGlowTex4.w (Level->GlowPulseRate, SetGlowReact's `rate`)
		// scales it. The level alone set the rate, so a strong alarm was
		// always a fast one and bright-and-slow could not be asked for (Red
		// Alert strobed). 1 is the original rate exactly; 0 stops the beat.
		float rate = (1.0 + 6.0 * lvl) * max(uGlowTex4.w, 0.0);
		float beat = 0.5 + 0.5 * sin(timer * rate * 6.2831853 * 0.35);
		mul *= 1.0 + uGlowTex4.y * lvl * (beat - 0.5) * 2.0;
	}

	return max(mul, 0.0);
}

//
// [BB] DARKNESS, PER FRAGMENT.
//
// A darkness mod scales each SECTOR's colour: one multiplier, one room, wall
// to wall. That was correct when a sector's light level was the only lever
// there was. It stopped being correct the moment a band of light could be
// measured per pixel -- and the tell was that the reveal worked by
// multiplying back UP what the darkness had multiplied DOWN. Two features
// politely undoing each other and calling the result a lighting model.
//
// THE FOUR CURVES ARE UNCHANGED. Subtract, compress, cap brightest and deepen
// shadows, transcribed from the ZScript they came from, which took them
// verbatim from the original. Pre-gain lifts the input before the curve;
// min-light floors the result and post-gain lifts it after. Same arithmetic,
// same order, same numbers.
//
// What changes is the INPUT: the fragment's own light rather than the room's.
// That alone stops a large room being uniformly dim. Then two terms that a
// sector could never have expressed at all:
//
//   DISTANCE -- darkness deepening with range. This is the one that makes a
//   dark room feel like it has depth instead of like the brightness slider
//   went down, and it is one line here and impossible per sector.
//
//   HEIGHT -- dark pooling at floor level, or rising as a tide.
//
// Returns the fraction of light that survives, 0..1.
//
//   uDarkness   mode, adjust, min light, pre-gain
//   uDarkness2  post-gain, distance depth, distance range, -
//   uDarkness3  height depth, height reference, height range, -
//
float DarknessAt(float lightLevel)
{
	int dmode = int(uDarkness.x);
	if (dmode <= 0) return 1.0;

	// The curves work in Doom's 0-255 light, because that is what they were
	// written against and every one of their constants -- 256, 33, /8 -- is
	// in those units. Converting here rather than rescaling the curves keeps
	// them readable against the original.
	float base = lightLevel * 255.0;
	if (base <= 0.0) return 1.0;      // already black; nothing to scale

	float A = uDarkness.y;
	// Pre-gain, before the curve. Floored at 0 because the gain can be
	// negative (the slider reaches -128) and mode 4 feeds L to pow(), which
	// GLSL leaves undefined for a negative base -- NaN on most drivers, and a
	// NaN survives the clamp on mul below as black/white/flicker garbage.
	float L = max(base + uDarkness.w, 0.0);

	float outL;
	if (dmode == 1)                   // subtract -- a simple fade
	{
		outL = L - A;
	}
	else if (dmode == 2)              // compress -- the one that ignores base
	{
		outL = L * (1.0 - A / 256.0);
	}
	else if (dmode == 3)              // cap brightest -- dark rooms survive
	{
		outL = min(L, 256.0 - A);
	}
	else                              // deepen shadows -- exponential gamma
	{
		if (A <= 0.0) outL = L;
		else outL = (256.0 - pow(A, A / 256.0))
		          * pow(L / 256.0, 1.0 + (A / (33.0 - (A / 8.0))));
	}

	outL = max(outL, uDarkness.z);    // min light, a floor
	outL += uDarkness2.x;             // post-gain, a lift

	float mul = clamp(outL / base, 0.0, 1.0);

	// DISTANCE. Measured from the eye to this fragment, so it deepens with
	// range rather than with which room you are standing in.
	if (uDarkness2.y > 0.0)
	{
		float d = distance(pixelpos.xyz, uCameraPos.xyz) / max(uDarkness2.z, 1.0);
		mul *= 1.0 - uDarkness2.y * clamp(d, 0.0, 1.0);
	}

	// HEIGHT. Below the reference, dark pools; above it, nothing changes. The
	// range is how far below the reference it takes to reach full depth.
	if (uDarkness3.x > 0.0)
	{
		float below = (uDarkness3.y - pixelpos.y) / max(uDarkness3.z, 1.0);
		mul *= 1.0 - uDarkness3.x * clamp(below, 0.0, 1.0);
	}

	return clamp(mul, 0.0, 1.0);
}

//
// [BB] REAL BEAMS.
//
// A laser in Doom is usually a sprite, or a chain of puffs spawned close
// enough together to read as a line. Both show what they are: the sprite
// lights nothing, and the chain stitches, gaps at long range, and costs an
// actor per segment.
//
// A beam is a SEGMENT. Light every pixel by its distance from that segment
// and you get the real thing -- the same idea as a sweep band, with the only
// difference being which distance is measured:
//
//   sweep band   distance from a POINT     length(p - origin)
//   beam         distance from a SEGMENT   length(p - closest(a,b))
//
// Because it is per pixel in world space, everything else follows without
// being asked for. The beam is continuous at any length with no repeat and no
// stitching. It wraps across floor, wall and ceiling as one unbroken object.
// And the surfaces near it brighten because they ARE near it, not because
// something also spawned a dynamic light to fake that.
//
// TWO FALLOFFS FROM ONE DISTANCE, and this is what separates a beam that
// looks hot from a bright line. A hard narrow CORE a couple of units across,
// and a wide soft HALO around it. One without the other reads as either a
// drawn line or a smear; together they read as something incandescent.
//
vec3 BeamLightAt(vec3 p)
{
	vec3 sum = vec3(0.0);
	int n = int(uBeamParams.x);
	if (n <= 0) return sum;

	for (int i = 0; i < 128; i++)
	{
		if (i >= n) break;

		vec3 a = uBeamA[i].xyz;
		vec3 b = uBeamB[i].xyz;

		float thick = max(uBeamA[i].w, 0.01);
		float soft  = max(uBeamB[i].w, 0.01);

		// CHEAP REJECT FIRST. A beam only lights within thick + soft*8 of
		// itself, so a fragment further than that from the segment's midpoint
		// -- plus half the segment's own length -- cannot possibly be lit by
		// it. That is a squared-distance compare against a bounding sphere:
		// about six operations, against roughly twenty-five for the solve
		// below.
		//
		// It matters because this runs for every beam on every fragment of
		// every draw. With eight beams standing in one doorway, the great
		// majority of the screen is outside all eight, and paying the full
		// closest-point solve to discover that eight times per pixel is what
		// takes a large display to its knees.
		vec3 mid = (a + b) * 0.5;
		float halfLen = length(b - a) * 0.5;
		float reach = thick + soft * 8.0 + 1.0;
		float cull = halfLen + reach;
		vec3 dm = p - mid;
		if (dot(dm, dm) > cull * cull) continue;

		vec3 ab = b - a;
		vec3 ap = p - a;

		// Closest point on the SEGMENT, not the infinite line -- the clamp is
		// what makes a beam end where it ends instead of lighting everything
		// along its axis out to the edge of the map.
		float t = clamp(dot(ap, ab) / max(dot(ab, ab), 0.0001), 0.0, 1.0);
		float d = length(ap - ab * t);

		float core = 1.0 - smoothstep(thick, thick + soft, d);
		float halo = 1.0 - smoothstep(thick, thick + soft * 8.0 + 1.0, d);

		// [BEAMLINES] Halo strength per line. An unstyled line carries the scene
		// value uBeamParams.y still holds, so it lights exactly as before.
		sum += uBeamCol[i].rgb * (core + halo * uBeamLook[i].y) * uBeamCol[i].w;
	}
	return sum;
}

//
// [BB] THE BEAM SEEN IN THE AIR -- emissive glow, no light and no geometry.
//
// BeamLightAt above lights SURFACES near a beam. That is not the same as
// seeing the beam: in a clear room it gives you a bright patch where the beam
// lands and nothing in between, which is a spotlight, not a laser.
//
// Making the beam itself visible normally means geometry -- a camera-facing
// quad strip with an additive texture -- and that brings everything a quad
// brings: a draw call, sorting against translucents, a seam where segments
// meet, and a beam that vanishes when viewed end-on.
//
// It does not need any of that. Asking "how close does my LINE OF SIGHT pass
// to this beam" is a segment-to-ray closest approach, which is a dozen lines
// of algebra with no object in it. Every fragment already knows where the eye
// is and where it is; that is enough.
//
// AND IT COMES OUT DEPTH-CORRECT FOR FREE. The closest approach has a distance
// ALONG the ray, so comparing that against the distance to the fragment says
// whether the beam passes in front of this pixel or behind it. A beam behind a
// wall is simply not drawn -- which is the one artefact the surface lighting
// genuinely cannot avoid, solved here as a side effect of the maths rather
// than by a shadow pass.
//
// It also feeds bloom without being told to. The bloom pass thresholds bright
// pixels, and a beam core writes values well past white, so the glow blooms
// exactly as an emissive thing should.
//
vec3 BeamAirGlow(vec3 fragPos)
{
	vec3 sum = vec3(0.0);
	int n = int(uBeamParams.x);
	if (n <= 0 || uBeamParams.w <= 0.0) return sum;

	vec3 eye = uCameraPos.xyz;
	vec3 toFrag = fragPos - eye;
	float fragDist = length(toFrag);
	if (fragDist < 0.001) return sum;
	vec3 dir = toFrag / fragDist;

	for (int i = 0; i < 128; i++)
	{
		if (i >= n) break;

		// [BEAMLINES] A line with no air glow of its own is skipped. uBeamParams.w
		// is the LARGEST uploaded air glow, so the early-out above still skips
		// the whole loop when nothing glows; this catches the rest one line at a
		// time -- including beams r_beams_drawn hands to the drawn-line path. With
		// every line on the scene look this never fires, as before.
		if (uBeamLook[i].x <= 0.0) continue;

		vec3 a = uBeamA[i].xyz;
		vec3 b = uBeamB[i].xyz;

		// SAME REJECT, ON THE RAY. A view ray can only see this beam's glow
		// if it passes within reach of it, and the cheapest sufficient test
		// is the distance from the beam's midpoint to the ray. One cross
		// product against the full closest-approach solve below, which has a
		// division in it and cannot be skipped once entered.
		float thick = max(uBeamA[i].w, 0.01);
		float soft  = max(uBeamB[i].w, 0.01);
		vec3 mid = (a + b) * 0.5;
		float cull = length(b - a) * 0.5 + thick + soft * 6.0 + 1.0;
		vec3 em = mid - eye;
		float along = clamp(dot(em, dir), 0.0, fragDist);
		vec3 perp = em - dir * along;
		if (dot(perp, perp) > cull * cull) continue;

		vec3 v = b - a;
		vec3 w = eye - a;

		float bb = dot(dir, v);
		float cc = dot(v, v);
		float dd = dot(dir, w);
		float ee = dot(v, w);
		float den = cc - bb * bb;      // dot(dir,dir) is 1

		float sc, tc;
		if (abs(den) < 0.0001)
		{
			// Looking straight down the beam. Any point does; take the near
			// end, which keeps an end-on beam a bright dot instead of the
			// division blowing up.
			sc = -dd;
			tc = 0.0;
		}
		else
		{
			sc = (bb * ee - cc * dd) / den;
			tc = (bb * -dd + ee) / den;
		}

		// Clamp to the ray in front of the eye, no further than the surface
		// this pixel is on -- that clamp IS the depth test -- and to the
		// segment, so the glow ends where the beam ends.
		sc = clamp(sc, 0.0, fragDist);
		tc = clamp(tc, 0.0, 1.0);

		float dist = length((eye + dir * sc) - (a + v * tc));


		// ---- WHAT HAPPENS ALONG THE BEAM ------------------------------
		//
		// tc is where on the segment we are, 0 at the muzzle and 1 at the
		// impact. It fell out of the closest-approach solve, so everything
		// below is arithmetic on a number already in hand.
		//
		// TAPER. A perfectly parallel-sided beam reads as a drawn line. Real
		// glare is tighter at the aperture and blooms toward what it hits.
		//
		// [BEAMLINES] Taper, flare, halo and air glow are read PER LINE from
		// uBeamLook[i] (x air glow, y halo, z taper, w flare). A line with no
		// style of its own carries the scene values the old uniforms held, so
		// the arithmetic below is unchanged for it. Scroll stays scene-wide.
		// drawnlines.fp repeats this block for one line; change both together.
		float bw = mix(1.0 - uBeamLook[i].z, 1.0, tc);
		thick *= bw;
		soft  *= bw;

		float bright = 1.0;

		// SCROLL. Energy travelling muzzle-to-impact. Without it a held beam
		// is completely static and the eye stops believing it is carrying
		// anything -- this is the single thing that makes it read as a beam
		// under load rather than a stick of light.
		if (uBeamFX.y > 0.0)
		{
			float along = tc * length(v);
			float s = sin(along * 0.06 - timer * uBeamFX.x);
			bright *= 1.0 + uBeamFX.y * s;
		}

		// IMPACT FLARE. A beam that simply stops looks unfinished; the far
		// end is where the energy is actually going, so it is the brightest
		// part of the whole thing.
		if (uBeamLook[i].w > 0.0)
			bright += uBeamLook[i].w * pow(clamp(tc, 0.0, 1.0), 8.0);

		// The same two-falloff shape the surface light uses, so the beam in
		// the air and the light it casts agree about how thick it is.
		float core = 1.0 - smoothstep(thick * 0.5, thick + soft, dist);
		float halo = 1.0 - smoothstep(thick, thick + soft * 6.0 + 1.0, dist);

		sum += uBeamCol[i].rgb * (core * 1.6 + halo * uBeamLook[i].y)
			* uBeamCol[i].w * uBeamLook[i].x * bright;
	}
	return sum;
}

//
// [BB] THE LATTICE SEEN IN THE AIR, INSIDE A SWEEP BAND.
//
// SweepFillAt patterns the band where it lands on a SURFACE. That is a grid
// painted on the walls: it wraps corners, and there is nothing between them.
// Walk into it and you walk through a picture of a laser grid.
//
// The beam system draws real lines in the air, and it caps at a fixed count
// (eight when this was written; MAX_BEAMS is 128 now) -- which
// is a fence, not a screen door, and raising the cap makes it worse because
// each beam is another segment solve for every fragment.
//
// SO DO NOT DRAW LINES. DRAW A LATTICE.
//
// A bar band is a PLANE. A view ray crosses a plane at exactly one point. So:
// find that point, evaluate a repeating grid there, and draw. Four lines by
// four and four hundred by four hundred cost exactly the same -- one
// intersection and one pattern lookup -- because a repeating pattern is
// fract(), not a loop.
//
// That is the whole trick, and it is the same one the sweep itself uses: ask
// each pixel a question about where it is, instead of building the thing out
// of objects.
//
// [round2 C5] Rings and shells are handled too. This used to say a ray meets a
// cylinder in two places and the near one needs a quadratic, so only the bars
// were drawn -- and five RS_Sweeps presets (Wall of Colour, Hologram,
// Interference, Bloodrush, Carnival) asked for air on a ring or shell and got
// nothing. SweepShellCrossing (beside SweepPlaneAxis) solves that quadratic and
// takes the nearer stretch of the wall.
//
// Forward declaration. SweepLineAxisAA is defined further down, beside the
// surface fill it was written for, and GLSL will not call a function it has
// not seen. Declaring it here rather than moving the definition keeps the two
// lattice paths -- painted and in the air -- next to the code they belong to.
float SweepLineAxisAA(float coord, float spacing, float width, float soft, float t, float aa, float wrapCount);

// [BB] Returns the light the lattice ADDS, and writes how much of the view it
// OCCLUDES into occOut with the colour to occlude toward in occColOut.
//
// Additive alone can only ever brighten, so a band could be a wall of light and
// never a wall of darkness -- adding cannot subtract. A band whose draw mode is
// CRUSH now blends the scene toward its own colour instead of adding to it,
// which is the same thing crush already means on a surface, applied in the air.
//
// That is what makes a solid band something you cannot see through: black to
// hide what is coming, or any colour for a wall of it.
vec3 SweepAirLattice(vec3 fragPos, out float occOut, out vec3 occColOut)
{
	vec3 sum = vec3(0.0);
	occOut = 0.0;
	occColOut = vec3(0.0);
	if (uSweepCount <= 0) return sum;
	if (uSweepAir.x <= 0.0) return sum;
	// [round2 A3] No global "both spacings are 0" early-out here any more. A
	// solid slab (fill 3) and a crush wall need no lines at all, and that test
	// made them draw nothing whenever both spacing sliders were at 0. Each band
	// decides for itself below, by its own fill.

	vec3 eye = uCameraPos.xyz;
	vec3 toFrag = fragPos - eye;
	float fragDist = length(toFrag);
	if (fragDist < 0.001) return sum;
	vec3 dir = toFrag / fragDist;

	for (int sb = 0; sb < 8; sb++)
	{
		if (sb >= uSweepCount) break;

		vec4 sband = uSweepBands[sb];
		int bandpack = int(sband.w);
		int bmode = bandpack & 15;
		// [round2 B2] MASKED: the word is now drawmode + 16*fill + 256*passed,
		// so an unmasked shift would read the passed bit as fill 16.
		int bfill = (bandpack >> 4) & 15;
		if (bmode <= 0) continue;

		// AIR STRENGTH IS ITSELF THE SWITCH.
		//
		// This used to also require a per-band fill mode, so asking for lasers
		// in the air meant setting two unrelated things on two parts of one
		// page, and setting only the obvious one drew nothing at all. A band
		// that is drawing, in a sweep whose air lattice is on, means lasers --
		// the grid is the default because it is the only answer that is ever
		// wanted from those two facts.
		//
		// A band that explicitly asks for dots or a solid slab still gets them.
		if (bfill <= 0) bfill = 1;

		// [round2 A3] SKIP PER BAND, BY WHAT THE FILL ACTUALLY NEEDS. Lines
		// (grid 1, dots 2) need a spacing in at least one axis; pickets (4) run
		// along U only; a solid slab (3) needs no spacing at all and is never
		// skipped, so a crush wall draws with both spacing sliders at 0.
		if ((bfill == 1 || bfill == 2) && uSweepFill.x <= 0.0 && uSweepFill.y <= 0.0) continue;
		if (bfill == 4 && uSweepFill.x <= 0.0) continue;

		int shape = int(uSweepBandOrigin[sb].w);
		vec3 o = uSweepBandOrigin[sb].xyz;
		float radius = sband.x;
		float thick = max(sband.y, 1.0);

		// [round2 A4] HOW FAR THE SLAB REACHES EACH SIDE OF THE BAND'S LINE.
		//
		// This was max(thick, 1) * 0.5 either side -- half of what the painted
		// band lights. SweepBandAttenAt lights out to thick on both sides and to
		// |uSweepTrail| on the passed side, so the air slab now takes the same
		// two widths: the ahead face widens only for a negative trail, the
		// behind face only for a positive one (its `sbehind` rule).
		float airTrail = abs(uSweepTrail);
		float aheadW  = (uSweepTrail < 0.0 && airTrail > thick) ? airTrail : thick;
		float behindW = (uSweepTrail >= 0.0 && airTrail > thick) ? airTrail : thick;

		// Which axis the plane is perpendicular to, and which way it travels.
		// [round2 A2] From SweepPlaneAxis, so the signed crossings 6-9 get an
		// air pattern too (they fell to `continue` here before) and 5 keeps its
		// sign instead of being mirrored onto the eye's side.
		int axis;
		float away;
		SweepPlaneAxis(shape, axis, away);

		// Where along the view ray the band is: [t0, t1].
		float t0 = 0.0;
		float t1 = 0.0;
		if (axis < 0)
		{
			// [round2 C5] RINGS AND SHELLS IN THE AIR. A ray can cross their
			// wall twice; SweepShellCrossing takes the nearer visible stretch.
			if (shape != 1 && shape != 4) continue;
			if (!SweepShellCrossing(shape, eye, dir, o, radius, aheadW, behindW, fragDist, t0, t1)) continue;
		}
		else
		{
			float planeAxisEye = eye[axis];
			float planeAxisDir = dir[axis];
			float planeAt = o[axis];

			// Only the unsigned bars (2, 3) sit at +radius AND -radius from the
			// origin. Test whichever side the eye is on -- that is the one coming
			// at you rather than the one already gone past. A signed shape is one
			// plane and keeps the direction SweepPlaneAxis gave it.
			if (away == 0.0) away = (planeAxisEye >= planeAt) ? 1.0 : -1.0;

			// Parallel view: the ray never crosses, so there is nothing to draw.
			if (abs(planeAxisDir) < 0.0001) continue;

			// THE BAND IS A SLAB, NOT A SHEET, so solve for the whole crossing
			// rather than one plane. Where the ray enters its front face, where it
			// leaves the back, and how much of that is actually in front of the
			// pixel we are shading.
			//
			// The previous version intersected the centre plane and then measured
			// how far the hit was from that plane -- which is zero by construction,
			// every time. The softening it was reaching for never happened, and a
			// grid clipped by a wall popped out of existence instead of fading.
			//
			// [round2 A4] The faces are built in signed distance along `away`:
			// ahead at radius + aheadW, behind at radius - behindW.
			float faceAhead  = planeAt + away * (radius + aheadW);
			float faceBehind = planeAt + away * (radius - behindW);
			float tA = (faceBehind - planeAxisEye) / planeAxisDir;
			float tB = (faceAhead - planeAxisEye) / planeAxisDir;
			t0 = max(min(tA, tB), 0.0);
			t1 = min(max(tA, tB), fragDist);
		}
		if (t1 <= t0) continue;   // entirely behind you, or entirely behind a wall

		// Sample at the middle of the crossing, and weigh by how much of the
		// slab survived the clip. A grazing view crosses more of it and pins at
		// full; a crossing half eaten by geometry fades out instead of popping.
		float t = 0.5 * (t0 + t1);
		float slab = clamp((t1 - t0) / (aheadW + behindW), 0.0, 1.0);

		vec3 hit = eye + dir * t;

		// [BB] THE ROOM GATE.
		//
		// Everything above builds a plane with no extent -- perpendicular to
		// one axis, infinite in the other two -- so without this the grid
		// stood everywhere that plane reached, and a window looking toward it
		// showed lasers in a room the sweep had never entered.
		//
		// Tested at the HIT rather than at fragPos: the question is where the
		// lattice itself is hanging in the air, not what surface happens to be
		// behind it. A grid seen through a doorway is outside the room even
		// though the wall beyond it is inside one.
		//
		// Max.w is the switch. A level that never publishes a room leaves it
		// zero and pays one compare per band.
		float roomFade = 1.0;
		if (uSweepRoomMax.w > 0.0)
		{
			float soft = max(uSweepRoomMin.w, 1.0);

			// Distance OUTSIDE the box on each axis, zero when inside. The
			// max of the three is how far out the point is overall, so a
			// corner fades on its true distance rather than three times over.
			vec3 outv = max(uSweepRoomMin.xyz - hit, hit - uSweepRoomMax.xyz);
			float outside = max(max(outv.x, outv.y), max(outv.z, 0.0));

			roomFade = 1.0 - smoothstep(0.0, soft, outside);
			if (roomFade <= 0.0) continue;
		}

		// The same two tangent axes the surface fill uses, so the lattice in
		// the air and the lattice on the wall line up exactly rather than
		// being two grids that nearly agree.
		// [round2 A2] Keyed on the plane's axis, not the shape id, so 6/8 take
		// (z, y) like 2 and 7/9 take (x, y) like 3. They used to fall through to
		// the (x, z) fallback, which is 5's plane, not theirs.
		// [round2 C5] A ring or shell takes SweepArcFrame, the surface fill's
		// own (arc, height) frame, with its snapped spacing and wrap-safe
		// antialias widths (C2). Plane bands keep aaU/aaV at -1, which leaves
		// SweepLineAxisAA taking fwidth exactly as SweepLineAxis did.
		vec2 uv;
		float spacingU = uSweepFill.x;
		float wrapU = 0.0;
		float aaU = -1.0;
		float aaV = -1.0;
		if (axis < 0)       SweepArcFrame(hit, o, radius, uv, spacingU, wrapU, aaU, aaV);
		else if (axis == 0) uv = vec2(hit.z, hit.y);
		else if (axis == 2) uv = vec2(hit.x, hit.y);
		else                uv = vec2(hit.x, hit.z);

		float tt = timer;
		uv.x += tt * uSweepFill2.y;
		if (uSweepFill2.x != 0.0)
		{
			float a = radians(uSweepFill2.x);
			float cs = cos(a), sn = sin(a);
			uv = vec2(uv.x * cs - uv.y * sn, uv.x * sn + uv.y * cs);
			SweepRotateAA(cs, sn, aaU, aaV);
		}

		float cov;
		if (bfill == 4)
		{
			// [BB] PICKETS -- bars floor to ceiling, measured by the room.
			//
			// The other fill modes are a pattern in absolute world space that
			// the room is then cut out of, which is why they read as wallpaper
			// laid over a corridor rather than as something standing in it.
			// This one takes its spacing FROM the room, so a narrow corridor
			// gets a tight ladder and a hall gets a wide one, and neither can
			// look like the same grid at a different crop.
			//
			// No vertical term at all. A bar unbroken from floor to ceiling
			// gets its height from the geometry for free and can never be
			// mistaken for a grid.
			//
			// [round2 C1] The room snap lives in SweepPicketSpacing now, shared
			// with the painted pickets. A ring or shell snaps round its own
			// circumference instead (SweepArcFrame), since a room span means
			// nothing to a curved wall.
			float across = (axis < 0) ? spacingU : SweepPicketSpacing(axis);
			cov = SweepLineAxisAA(uv.x, across, uSweepFill.z, uSweepFill.w, tt, aaU, wrapU);
		}
		else
		{
			float lu = SweepLineAxisAA(uv.x, spacingU, uSweepFill.z, uSweepFill.w, tt, aaU, wrapU);
			float lv = SweepLineAxisAA(uv.y, uSweepFill.y, uSweepFill.z, uSweepFill.w, tt, aaV, 0.0);
			cov = (bfill == 2) ? min(lu, lv) : max(lu, lv);
			if (bfill == 3) cov = 1.0;
		}

		float amt = cov * slab * uSweepColors[sb].a * uSweepAir.x * roomFade;

		// CRUSH OCCLUDES INSTEAD OF ADDING. Same meaning the mode already has
		// on a surface -- take light away rather than put it in -- so a solid
		// slab in crush is a wall you cannot see through.
		// [round2 A1] bmode, NOT the whole word. .w is drawmode + 16*fill, and
		// RS_Sweeps always sends a fill, so crush with a solid slab packed as 51
		// and never equalled 3: crush walls added their colour instead of
		// occluding, and Wall of Darkness / Wall of Fog drew no wall.
		if (bmode == 3)
		{
			float o = clamp(amt, 0.0, 1.0);
			if (o > occOut) { occOut = o; occColOut = uSweepFillCol.rgb; }
		}
		else
		{
			sum += uSweepFillCol.rgb * amt;
		}
	}
	return sum;
}

//
// [BB] FOG WITH A TOP.
//
// Sector fog is a distance tint on SURFACES -- the further a wall is, the more
// it blends toward the fog colour. Nothing is simulated in the air, which is
// why it has no shape: no ceiling, no thickness you can stand in, and no way
// to be brighter where a light passes through it. You cannot be knee deep in
// it, because it has no knees.
//
// This is a horizontal slab of participating medium with a world-space top,
// and it is solved ANALYTICALLY rather than raymarched. For a flat-topped slab
// the answer is closed form: find how much of the eye-to-fragment ray lay
// below the ceiling, and fog by that length. No loop, no step count, no
// undersampling artefacts, and the cost is a handful of ALU.
//
//   uFogSlab       x top Z, y density per 1000 units, z soft edge, w scatter
//   uFogSlabColor  rgb, w wake strength
//   uFogSlabWake   xyz wake point, w radius
//
// THE SOFT TOP IS NOT DECORATION. A hard cut at the ceiling reads as a plane
// of coloured glass lying across the room. Fading the density over the last
// few units turns it into a surface you can look down at, which is the whole
// effect being asked for.
//
// SCATTER uses the flashlight cone the renderer already knows about, so mist
// inside the beam brightens without a second trace. That is the difference
// between fog you are standing in and fog you are standing in WITH A TORCH.
//
// Returns rgb = the fog's own colour for this pixel, a = how much of it.
//
//
// [BB] Two-dimensional value noise, for the density field and the tendril
// lattice. Deliberately the cheap kind: four hashes and three mixes. Fog does
// not need gradient noise -- it needs "not the same everywhere", and the eye
// cannot tell which sort of noise made a mist bank.
//
float GITDHash21(vec2 p)
{
	vec3 q = fract(vec3(p.xyx) * 0.1031);
	q += dot(q, q.yzx + 33.33);
	return fract((q.x + q.y) * q.z);
}

float GITDNoise2(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(GITDHash21(i), GITDHash21(i + vec2(1, 0)), f.x),
	           mix(GITDHash21(i + vec2(0, 1)), GITDHash21(i + vec2(1, 1)), f.x), f.y);
}

//
// [BB] TENDRILS, AS A LATTICE.
//
// Wisps rising off the mist. The obvious way is one object each, and eight of
// them look like eight sticks; what a room wants is hundreds.
//
// So this is the tornado's own maths -- distance from a vertical axis, faded
// by height -- evaluated once per CELL of a fract() grid rather than once per
// object. Four hundred tendrils cost what one costs, which is the same trick
// the sweep's laser lattice uses and for the same reason.
//
// EACH TENDRIL STAYS INSIDE ITS OWN CELL. The hashed offset is limited so the
// column can never cross a cell boundary, which is what lets this look at one
// cell instead of the nine around it -- a ninefold saving for a constraint
// nobody can see, since the offset still moves it off the lattice enough to
// stop the grid reading as a grid.
//
//   uFogTendril   x cell spacing, y radius, z height, w density
//   uFogTendril2  x rise speed, y phase spread, z lean, w taper
//
float FogTendrilAt(vec3 p, float baseY)
{
	if (uFogTendril.w <= 0.0) return 0.0;

	float cell = max(uFogTendril.x, 8.0);
	float rad  = min(max(uFogTendril.y, 1.0), cell * 0.45);
	float tall = max(uFogTendril.z, 1.0);

	// How far up its own column this fragment is. Above the top there is
	// nothing to compute, and that is most of the screen looking forward.
	float h = (p.y - baseY) / tall;
	if (h < 0.0 || h > 1.0) return 0.0;

	vec2 cellId = floor(p.xz / cell);
	vec2 local  = p.xz - (cellId + 0.5) * cell;

	// A stable offset per cell, and a stable phase, so a given wisp is always
	// the same wisp -- it does not swim around when you move.
	float hx = GITDHash21(cellId);
	float hz = GITDHash21(cellId + 17.31);
	float hp = GITDHash21(cellId + 91.7);

	float room = cell * 0.5 - rad;
	vec2 axis = vec2(hx - 0.5, hz - 0.5) * 2.0 * room;

	// LEAN, growing with height, so a tendril curls instead of standing to
	// attention. Each one leans its own way, from its own hash.
	float sway = timer * uFogTendril2.x + hp * 6.2831853 * uFogTendril2.y;
	axis += vec2(sin(sway), cos(sway * 0.83)) * uFogTendril2.z * h;

	float r = length(local - axis);

	// TAPER. A tendril that is the same width at the top is a pipe. Narrowing
	// as it climbs is most of what makes it read as something drifting up out
	// of the mist rather than something planted in it.
	float rr = rad * pow(1.0 - h, uFogTendril2.w * 0.5) ;
	if (rr <= 0.001 || r > rr) return 0.0;

	float core = 1.0 - smoothstep(rr * 0.25, rr, r);

	// Fades in off the surface and out at the top, so neither end is a cut.
	core *= smoothstep(0.0, 0.18, h);
	core *= 1.0 - smoothstep(0.55, 1.0, h);

	// Each rises on its own clock, so the field breathes instead of pulsing.
	core *= 0.55 + 0.45 * sin(timer * uFogTendril2.x * 1.7 + hp * 6.2831853);

	return max(core, 0.0) * uFogTendril.w;
}

// [BB] The height of a sector plane at a point. Same expression main.vp uses
// to find a fragment's distance from a glow -- the plane equation, so it is
// exact on a slope and not just on a step.
float FogPlaneAt(vec4 pl, vec3 p)
{
	return (pl.w + pl.x * p.x + pl.y * p.z) * pl.z;
}

vec4 FogSlabAt(vec3 fragPos)
{
#ifdef SCENE_LIGHT_MASK
	gFogSlabPinnedLight = vec3(0.0);	// [LIGHTMASK] set again below when beams light the mist
#endif
	// EITHER SHAPE IS ENOUGH TO MAKE THIS WORTH RUNNING, and that is the whole
	// of what "independent" means here. The funnel is not a feature of the
	// layer -- it is a second shape made of the same mist, and gating it on the
	// layer's density made a tornado in clear air impossible to ask for.
	//
	// They keep the ray setup, the torch, the glow pickup and the compositing
	// in common, because those are properties of MIST and not of either shape.
	// What they do not share any more is whether they exist, how thick they
	// are, and what colour they are.
	bool haveSlab = uFogSlab.y > 0.0;
	bool haveTorn = uTornado2.z > 0.0;
	bool haveTend = uFogTendril.w > 0.0;

	// IGNITE HAS TO SURVIVE AN EMPTY ROOM, and this line is why it did not.
	//
	// The claim everywhere else -- in the menu, the cvar note and the README
	// -- is that Ignite adds LIGHT rather than mist and therefore works in
	// clear air. It did not. The guard returned before the disturbance loop
	// whenever there was no slab, no funnel and no wisps, so an explosion in a
	// room with the fog switched off lit nothing at all and the documentation
	// was describing an intention.
	//
	// uFogBow.w was the one genuinely spare slot, and it is pushed outside the
	// slab gate, which is what this needs -- a count that only exists while the
	// fog is on would be no use to the case it is here to rescue.
	bool haveDist = uFogBow.w > 0.0;
	if (!haveSlab && !haveTorn && !haveTend && !haveDist) return vec4(0.0);

	vec3  eye  = uCameraPos.xyz;
	float topZ = uFogSlab.x;
	float soft = max(uFogSlab.z, 0.001);

	// THE SURFACE MOVES.
	//
	// A flat top is a horizontal plane, and once you can see it clearly that
	// is exactly what it reads as -- a sheet, not a body of mist. Making the
	// height a function of position and time instead of a constant is the same
	// move the glow wave makes on a glow's edge, for the same reason.
	//
	// TWO WAVES AT AN ANGLE. One sine corrugates: parallel ridges marching in
	// a single direction, which reads as machinery. A second at an angle and a
	// different wavelength makes them interfere, and interference is what
	// looks like a surface rolling rather than a pattern scrolling.
	//
	// Sampled SEPARATELY at the eye and at the fragment, because the height at
	// your feet and the height across the room are genuinely different now --
	// which is the whole point, and is what makes the boundary undulate as you
	// look along it.
	// ---- THE SURFACE FOLLOWS THE ARCHITECTURE -------------------------
	//
	// A single world Z is flat across the whole map: knee deep in one room and
	// overhead in the pit next door, and it does not climb a staircase. What
	// "fog on the floor" actually means is a constant height ABOVE THE GROUND.
	//
	// The floor and ceiling planes are already here per draw -- the vertex
	// shader uses the same two to find how far a fragment is from a glow. The
	// height of a plane at a point is one multiply-add each, and it is exact
	// on slopes as well as steps because it is the plane equation and not a
	// sampled height.
	//
	// THE AMOUNT IS THE GENTLENESS. At 1 the surface tracks every step
	// exactly; at 0.3 it rises three units for every ten the floor does, so a
	// staircase reads as a slope rather than as a flight of steps. Fog that
	// steps looks like geometry. Fog that lags looks like weather.
	//
	// The EYE end is pushed as a number rather than read from these planes,
	// and that is not a shortcut -- the planes describe the FRAGMENT's sector.
	// Using them for the eye would mean that looking at a wall on the floor
	// above raised the fog around your head to match it.
	//
	// [RS fork] THE FULL FLOOR HEIGHT IS ADDED; THE DIAL ONLY BLENDS WHICH
	// FLOOR. This used to be `floorZ * amount` -- a scale on ABSOLUTE world Z --
	// so top 56 at 0.35 put the surface under any floor above ~86 units and
	// hundreds of units deep in a pit, and "constant depth underfoot" was only
	// true at 1. Now the edge is `value + floor`, where floor is the eye's floor
	// blended toward this fragment's floor by the amount: at 1 every step is
	// tracked exactly, at 0.3 a step ten units up raises the fog three -- the
	// same gentleness, measured from where you stand instead of from world 0.
	// At the eye the blend is the eye floor itself, so depth underfoot is the
	// value on any floor. Ceiling-following is the same with the ceiling.
	float topOffFrag = 0.0, topOffEye = 0.0;
	if (uFogFollow.x > 0.0)
	{
		topOffEye  = uFogFollow.z;
		topOffFrag = mix(uFogFollow.z, FogPlaneAt(uGlowBottomPlane, fragPos), uFogFollow.x);
	}
	else if (uFogFollow.x < 0.0)
	{
		topOffEye  = uFogFollow.w;
		topOffFrag = mix(uFogFollow.w, FogPlaneAt(uGlowTopPlane, fragPos), -uFogFollow.x);
	}
	topZ += topOffFrag;

	float topEye  = topZ - topOffFrag + topOffEye;
	float topFrag = topZ;
	if (uFogSurf.x > 0.0)
	{
		float wl = max(uFogSurf.y, 1.0);
		float t  = timer * uFogSurf.z;
		float cr = uFogSurf.w;

		topEye  += uFogSurf.x * (sin(eye.x / wl + t)
		         + cr * sin((eye.z * 0.77 + eye.x * 0.31) / wl - t * 1.31));
		topFrag += uFogSurf.x * (sin(fragPos.x / wl + t)
		         + cr * sin((fragPos.z * 0.77 + fragPos.x * 0.31) / wl - t * 1.31));
	}

	// AND IT HAS A BOTTOM.
	//
	// Without one the slab is a half-space -- everything below a height --
	// which can only ever be fog lying on the floor. With one it is a LAYER,
	// and that single change is four effects rather than one:
	//
	//   floor fog     bottom far below, top at the knee (the old behaviour)
	//   CEILING FOG   bottom near the ceiling, top above it
	//   a floating band at chest height, which was not expressible at all
	//   DRAIN and FILL, by animating one edge toward the other
	//
	// The bottom takes the same swell as the top, so a ceiling layer's
	// UNDERSIDE undulates -- which is the surface you actually see from below,
	// and animating only the top would have left it a flat plate.
	// The bottom edge follows on its own terms, and it has to: CEILING FOG is
	// the bottom edge tracking the ceiling while the top sits above it. One
	// shared follow setting could express floor fog or ceiling fog but never
	// a band that does both, and never the layer walking upstairs intact.
	float botZ = uFogSlab2.x;
	float botOffFrag = 0.0, botOffEye = 0.0;
	// [RS fork] Same rule as the top: full floor (or ceiling) height, with the
	// amount blending the eye's toward the fragment's. See the top's note.
	if (uFogFollow.y > 0.0)
	{
		botOffEye  = uFogFollow.z;
		botOffFrag = mix(uFogFollow.z, FogPlaneAt(uGlowBottomPlane, fragPos), uFogFollow.y);
	}
	else if (uFogFollow.y < 0.0)
	{
		botOffEye  = uFogFollow.w;
		botOffFrag = mix(uFogFollow.w, FogPlaneAt(uGlowTopPlane, fragPos), -uFogFollow.y);
	}

	float botEye = botZ + botOffEye, botFrag = botZ + botOffFrag;
	if (uFogSurf.x > 0.0)
	{
		botEye  += topEye  - topZ - topOffEye + topOffFrag;
		botFrag += topFrag - topZ;
	}

	// VERTICAL HOLD.
	//
	// With a period set, the layer REPEATS up the room -- a stack of them, all
	// rolling together, wrapping at the top and coming back in at the bottom.
	// That is the old television fault, and it is one mod() away from the
	// single-layer case rather than a second system.
	//
	// The whole stack costs exactly what one layer costs, because a repeating
	// thing is arithmetic, not a loop. Same reason the lattice can be a screen
	// door.
	float period = uFogSlab2.y;
	float dEye, dFrag;

	if (period > 1.0)
	{
		float thickness = max(topEye - botEye, 0.0);
		float roll = timer * uFogSlab2.z;

		// Wrap each end's height into one period, measured from the bottom,
		// then test it against the layer's own thickness. Sampled separately
		// at the eye and the fragment so the stack has depth rather than
		// being a flat repeat.
		float yEye  = mod(eye.y - botEye - roll, period);
		float yFrag = mod(fragPos.y - botFrag - roll, period);

		dEye  = smoothstep(thickness + soft, thickness - soft, yEye)
		      * smoothstep(-soft, soft, yEye);
		dFrag = smoothstep(thickness + soft, thickness - soft, yFrag)
		      * smoothstep(-soft, soft, yFrag);
	}
	else
	{
		// Inside the layer is below the top AND above the bottom. The default
		// bottom sits far below any map, so the second term is 1 and the old
		// half-space behaviour is byte for byte what it was.
		dEye  = smoothstep(topEye + soft, topEye - soft, eye.y)
		      * smoothstep(botEye - soft, botEye + soft, eye.y);
		dFrag = smoothstep(topFrag + soft, topFrag - soft, fragPos.y)
		      * smoothstep(botFrag - soft, botFrag + soft, fragPos.y);
	}

	// Average occupancy along the segment. Exact for a linear ramp, and the
	// error against a true integral through the smoothstep is far below what
	// the eye can see in fog.
	// Nothing of the LAYER is here -- but the funnel may still be, and it is
	// measured from a different quantity entirely, so this can only bail when
	// there is no funnel either.
	float occupancy = 0.5 * (dEye + dFrag);
	if (occupancy <= 0.0 && !haveTorn && !haveTend && !haveDist) return vec4(0.0);

	float travel = distance(eye, fragPos) * occupancy;

	// WAKE. Inside the radius the mist has been disturbed and is thinner --
	// this is the trail you kick up walking through it. One point that lags
	// behind the player, because a trail that settles IS a point that follows
	// you slowly.
	//
	// STRETCHED ALONG THE WAY YOU ARE GOING. A disc is a hole you carry about
	// with you; an ellipse drawn out behind is a corridor you carve and leave.
	// Same one point and the same one radius -- the offset is just measured in
	// the frame of your own velocity before its length is taken.
	if (uFogSlabColor.w > 0.0 && uFogSlabWake.w > 0.0)
	{
		vec2 rel = fragPos.xz - uFogSlabWake.xz;
		float vlen = length(uFogWake2.xy);
		if (uFogWake2.z > 0.0 && vlen > 0.001)
		{
			vec2 vd = uFogWake2.xy / vlen;
			float along  = dot(rel, vd);
			float across = dot(rel, vec2(-vd.y, vd.x));
			// Only the trailing half stretches. Stretching both would put as
			// much cleared air in front of you as behind, which is a bubble
			// rather than a wake.
			float s = 1.0 + uFogWake2.z * clamp(-along / max(uFogSlabWake.w, 1.0), 0.0, 4.0);
			rel = vec2(along / s, across);
		}
		float w = 1.0 - smoothstep(0.0, uFogSlabWake.w, length(rel));
		travel *= 1.0 - uFogSlabColor.w * w;
	}

	// ---- DENSITY IS NOT ONE NUMBER ------------------------------------
	//
	// Uniform density is the single biggest tell that fog is a filter rather
	// than a substance: real mist banks up, thick in the corners and thin
	// across the open. One noise sample scaling the density fixes that, and
	// drifting the field slowly makes the banks move through a room on their
	// own without anything being animated.
	//
	// Sampled at the MIDPOINT of the eye-to-fragment segment, not at the
	// fragment. The fog for a pixel is an integral along that whole line, and
	// sampling at the far end makes the field appear pinned to the walls --
	// you would walk through a bank and see it stay where the geometry is.
	// Scaled per draw -- a courtyard and a cellar want opposite amounts of this,
	// and the sector picked which. 1 is the slab as configured.
	float dens = uFogSlab.y * uFogDensityScale;
	if (uFogNoise.y > 0.0)
	{
		vec2 mid = mix(eye.xz, fragPos.xz, 0.5) + uFogNoise.zw * timer;
		float n = GITDNoise2(mid * uFogNoise.x);
		// Around 1, not from 0, so the dial thins and thickens rather than
		// only ever taking mist away.
		dens *= mix(1.0, 0.35 + 1.3 * n, clamp(uFogNoise.y, 0.0, 1.0));
	}

	// ---- WHAT THE SWEEP DOES TO THE AIR --------------------------------
	//
	// A wall of light travelling through a room ought to move the room. The
	// band already knows its own distance function, so piling mist against the
	// leading face and thinning it behind costs one evaluation of maths that
	// is already written -- and a sweep through fog stops being a light and
	// becomes a physical event.
	if (uFogBow.x > 0.0 && uSweepCount > 0)
	{
		float bow = 0.0;
		for (int bb = 0; bb < 8; bb++)
		{
			if (bb >= uSweepCount) break;
			int bp = int(uSweepBands[bb].w);
			if ((bp & 15) <= 0) continue;

			int bs = int(uSweepBandOrigin[bb].w);
			vec3 bo = uSweepBandOrigin[bb].xyz;
			// [RS fork] SweepShapeDist, the band light's own measure. This
			// treated shell (4) and the signed crossings (6-9) as rings, so
			// the bulge sat nowhere near the visible band. Signed shapes stay
			// signed, so sd > 0 below still means "not yet reached".
			if (bs <= 0) continue;
			float here = SweepShapeDist(bs, fragPos, bo);

			// Signed distance from the band surface: ahead of it is positive.
			float sd = here - uSweepBands[bb].x;
			float w = max(uFogBow.y, 1.0);
			if (abs(sd) > w) continue;

			// Piled in front, scoured behind. The two are the same curve with
			// opposite signs, which is what makes it read as displacement
			// rather than as a glow travelling with the band.
			float f = 1.0 - abs(sd) / w;
			bow += (sd > 0.0) ? f * f : -f * f * uFogBow.z;
		}
		dens *= clamp(1.0 + bow * uFogBow.x, 0.0, 8.0);
	}

	// ---- DISTURBANCES --------------------------------------------------
	//
	// One primitive, five effects. Everything reactive the mist does -- a
	// gunshot ring, an explosion lighting it, a monster shouldering through
	// it, fog draining from a point -- is this loop with a different mode.
	//
	// Modes 0, 1 and 3 change how much mist is in the way and so are applied
	// to TRAVEL, before the exponential. Mode 2 adds light instead and is
	// gathered separately, further down, because a burning cloud is brighter
	// mist and not more of it.
	// Same early break as the glow feed above -- this is the loop that runs
	// over every fragment inside the fog volume, so walking empty slots here
	// is the most expensive place in the shader to do it.
	vec3 ignite = vec3(0.0);

	// [RS fork] IGNITE'S OWN COLOUR (SetFogIgniteColor), packed into
	// uFogWake2.w as 1 + 0xRRGGBB -- exact in a float, every value is below
	// 2^24 -- with 0 meaning unset. Unset keeps the old behaviour: ignite burns
	// in the gradient colour, which most presets leave black, which is why a
	// separate colour exists. Decoded once, outside the per-slot loop.
	vec3 igniteCol = uFogColor2.rgb;
	if (uFogWake2.w > 0.5)
	{
		float pk = floor(uFogWake2.w + 0.5) - 1.0;
		igniteCol = vec3(floor(pk / 65536.0), mod(floor(pk / 256.0), 256.0), mod(pk, 256.0)) / 255.0;
	}

	int ndisturb = int(uFogBow.w);
	for (int di = 0; di < 32; di++)
	{
		if (di >= ndisturb) break;

		float stren = uFogDisturbB[di].y;
		if (stren <= 0.0) continue;

		vec3  dp   = uFogDisturbA[di].xyz;
		float drad = uFogDisturbA[di].w;
		float age  = uFogDisturbB[di].x;
		float spd  = uFogDisturbB[di].z;
		int   mode = int(uFogDisturbB[di].w);

		if (mode == 1)
		{
			// RIPPLE. A ring travelling out at r = age * speed, and the mist
			// piles on the crest and thins in the trough -- which is what a
			// wave IS. A ring that only ever removed mist would read as an
			// expanding hole, and holes do not travel.
			float r = distance(fragPos.xz, dp.xz);
			float front = age * spd;
			float band = 1.0 - smoothstep(0.0, max(drad, 1.0), abs(r - front));
			if (band <= 0.0) continue;
			travel *= 1.0 + stren * band * sin((r - front) * 0.06);
		}
		else if (mode == 2)
		{
			// IGNITE. An expanding sphere of burning mist. Written into the
			// colour, so it feeds bloom on its own with nothing attached to
			// it, and it needs no density at all to be visible.
			float r = distance(fragPos, dp);
			float front = drad + age * spd;
			float shell = 1.0 - smoothstep(front * 0.35, front, r);
			if (shell <= 0.0) continue;
			ignite += igniteCol * shell * stren;
		}
		else
		{
			// DISC thins, GOUT thickens, and a GOUT grows. Same three lines
			// with a sign and a radius that may or may not depend on age.
			float rr = (mode == 3) ? drad + age * spd : drad;
			float r = distance(fragPos.xz, dp.xz);
			float w = 1.0 - smoothstep(0.0, max(rr, 1.0), r);
			if (w <= 0.0) continue;
			travel *= (mode == 3) ? (1.0 + stren * w) : (1.0 - clamp(stren, 0.0, 1.0) * w);
		}
	}

	float amount = 1.0 - exp(-dens * max(travel, 0.0) * 0.001);
	amount = clamp(amount, 0.0, 1.0);

	// ---- TENDRILS ------------------------------------------------------
	//
	// Added after the layer's own integral because a wisp is a local lump of
	// density and not a longer path through the layer. They rise off the
	// slab's top surface, which is why this needs topFrag rather than a
	// constant -- a tendril growing out of a rolling surface has to roll with
	// it or it hangs unattached in the air above the mist.
	if (haveTend)
		// SCALED WITH THE SLAB. The body honours the indoor/outdoor split and the
		// wisps did not, so a preset that is thick indoors and almost nothing
		// outdoors put full-strength tendrils standing in clear air the moment you
		// stepped outside.
		amount = clamp(amount + FogTendrilAt(fragPos, topFrag) * uFogDensityScale, 0.0, 1.0);

	// [RS fork] Starts BLACK when there is no slab and no wisps. Then col only
	// carries added light -- ignite, the torch, beams -- and must not start from
	// a slab colour that is not being drawn: clear-air ignite used to take the
	// last preset's colour, or the header's orange-red. Wisps are the slab's
	// own mist, so they keep its colour (uploaded outside the gate now).
	vec3 col = (haveSlab || haveTend) ? uFogSlabColor.rgb : vec3(0.0);

	// A SECOND COLOUR ACROSS THE LAYER'S OWN THICKNESS. Cold at the floor,
	// warm at the top, or the other way about. Measured against the layer
	// rather than against the world, so it follows the top as it swells and
	// means the same thing in a chest-high band as in knee-deep ground mist.
	if (uFogColor2.w > 0.0)
	{
		float span = max(topFrag - botFrag, 1.0);
		float up = clamp((fragPos.y - botFrag) / span, 0.0, 1.0);
		col = mix(col, uFogColor2.rgb, up * clamp(uFogColor2.w, 0.0, 1.0));
	}

	// SCATTER off the torch. Cheap: how far inside the beam cone this
	// fragment sits, using the cone the volumetric beam already describes.
	if (uFogSlab.w > 0.0 && uFogBeamPos.w > 0.0)
	{
		vec3 toFrag = fragPos - uFogBeamPos.xyz;
		float len = length(toFrag);
		if (len > 0.001)
		{
			float cosA = dot(toFrag / len, uFogBeamDir.xyz);
			float lit = smoothstep(uFogBeamCol.w, uFogBeamDir.w, cosA);
			// Along the beam, the SAME curve the air pass uses -- pow(1 - d/L,
			// falloff) -- so the mist glow dies out where the visible beam does.
			// Falloff arrives in uFogSlabExtra.z; 0 means nothing published it
			// and keeps the old linear fade. The beam's brightness (density,
			// after flicker) is premultiplied into uFogBeamCol.rgb on the CPU,
			// so a torch at Brightness 0 no longer lights the fog.
			float beamFall = uFogSlabExtra.z > 0.0 ? uFogSlabExtra.z : 1.0;
			lit *= pow(1.0 - clamp(len / uFogBeamPos.w, 0.0, 1.0), beamFall);
			col += uFogBeamCol.rgb * lit * uFogSlab.w;
		}
	}

	// The funnel's own accumulator. It is NOT added into the layer's, because
	// the two carry different colours and a single number cannot say which of
	// them a pixel's mist came from. Kept separate here and merged once, below.
	float tam = 0.0;

	// ---- TORNADOES -----------------------------------------------------
	//
	// A funnel you can stand inside, and it is the fog system rather than a
	// second one: density near a vertical axis instead of density below a
	// plane. The distance solve is the beam's, with the segment standing up.
	//
	// Three things turn a cylinder into a tornado, and none of them is noise:
	//
	//   RADIUS BY HEIGHT. Constant radius is a pillar. The profile is what
	//   gives it a waist -- tight at the ground, flaring up, on a curve rather
	//   than a straight taper.
	//
	//   SWIRL. Angle around the axis, plus height times twist, plus time times
	//   spin, through a sine. Spiral bands winding up the column, and THAT is
	//   what the eye reads as rotation. Without it you have a cone of haze.
	//
	//   LEAN. The axis is not vertical: it drifts with height, and that drift
	//   is itself a slow sine, so the column writhes. This is the difference
	//   between something alive and a traffic cone.
	//
	// Inside one you are surrounded on all sides with the bands wrapping past
	// you, and it takes the torch, the glow pickup and any beam crossing it,
	// because it is the same density the rest of this function is made of.
	//
	//   uTornado   x world X, y world Z(doom Y), z base height, w top height
	//   uTornado2  x base radius, y top radius, z density, w swirl depth
	//   uTornado3  x spin, y twist, z lean, w lean period
	//
	if (uTornado2.z > 0.0)
	{
		float baseY = uTornado.z;
		float topY  = uTornado.w;
		float h = clamp((fragPos.y - baseY) / max(topY - baseY, 1.0), 0.0, 1.0);

		if (fragPos.y >= baseY - 32.0 && fragPos.y <= topY + 32.0)
		{
			// LEAN, and it is a function of height, so the column BENDS
			// rather than sliding sideways as a whole. The offset is scaled
			// by h, which is what pins the foot of it to one spot on the
			// floor while the top wanders.
			//
			// Two axes at slightly different rates, so the top traces a
			// wobbling ellipse rather than a circle -- a circle reads as a
			// mechanism turning, and this is meant to read as weather.
			//
			// The period is in seconds, not radians: it is what the slider
			// says it is, so nobody has to know 2pi to use it. The h term
			// inside the sine is what makes the column bend along its
			// length instead of tilting like a rigid pole.
			float lt = timer * 6.2831853 / max(uTornado3.w, 0.1);
			vec2 axis = uTornado.xy
				+ vec2(sin(lt + h * 2.0), cos(lt * 0.83 + h * 2.0))
				* uTornado3.z * h;

			vec2 rel = fragPos.xz - axis;
			float r = length(rel);

			// The profile. pow rather than mix, because a straight taper is a
			// cone and the pinch near the ground is most of the silhouette.
			float radius = mix(uTornado2.x, uTornado2.y, pow(h, 0.55));

			if (r < radius)
			{
				// Densest at the wall of the funnel, hollow in the middle --
				// which is what lets you be INSIDE one and still see out.
				float shell = smoothstep(0.0, radius * 0.55, r);
				float edge  = 1.0 - smoothstep(radius * 0.8, radius, r);
				float d = shell * edge;

				// SWIRL.
				if (uTornado2.w > 0.0)
				{
					float ang = atan(rel.y, rel.x);
					float s = sin(ang * 3.0 + h * uTornado3.y - timer * uTornado3.x);
					d *= 1.0 + uTornado2.w * s;
				}

				// Fades out at both ends so it does not stop dead against the
				// floor or the ceiling.
				d *= smoothstep(baseY - 32.0, baseY + 48.0, fragPos.y);
				d *= 1.0 - smoothstep(topY - 96.0, topY + 32.0, fragPos.y);

				tam = clamp(tam + max(d, 0.0) * uTornado2.z, 0.0, 1.0);
			}
		}
	}

	// ---- TWO SHAPES, TWO COLOURS, ONE PIXEL ----------------------------
	//
	// The funnel gets its own colour and its own torch response, so a red
	// column can stand in blue ground mist and neither one has to be a tint of
	// the other. That is the whole request, and it costs one uniform.
	if (haveTorn)
	{
		vec3 tcol = uTornadoCol.rgb;

		// Its own scatter dial rather than the layer's, because the layer's is
		// pushed by SetFogSlab and is therefore zero whenever floor fog is off
		// -- which is exactly when a tornado standing alone needs it most.
		if (uTornadoCol.w > 0.0 && uFogBeamPos.w > 0.0)
		{
			vec3 toFrag = fragPos - uFogBeamPos.xyz;
			float len = length(toFrag);
			if (len > 0.001)
			{
				float cosA = dot(toFrag / len, uFogBeamDir.xyz);
				float lit = smoothstep(uFogBeamCol.w, uFogBeamDir.w, cosA);
				// Same falloff and brightness as the slab's torch glow above.
				float beamFall = uFogSlabExtra.z > 0.0 ? uFogSlabExtra.z : 1.0;
				lit *= pow(1.0 - clamp(len / uFogBeamPos.w, 0.0, 1.0), beamFall);
				tcol += uFogBeamCol.rgb * lit * uTornadoCol.w;
			}
		}

		// UNION, NOT SUM. Mist you are looking through twice does not become
		// twice as opaque -- what gets through is what gets through both, so
		// the survivals multiply and the coverages combine as a+b-ab. Adding
		// them instead would drive the overlap straight to solid the moment a
		// funnel crossed a fog layer, which is the one place it must not.
		float total = amount + tam - amount * tam;

		// Colour weighted by how much each shape actually contributed, so the
		// overlap reads as the two mists mingling rather than one winning.
		float wsum = amount + tam;
		col = (wsum > 0.0001) ? (col * amount + tcol * tam) / wsum : tcol;
		amount = total;
	}

	// AND BEAMS LIGHT THE MIST THEY CROSS.
	//
	// This is the shot worth having: a laser through knee-deep fog should be
	// visible as a shaft along its whole length, not just as a bright line on
	// whatever it eventually hits. Evaluated at the fragment rather than
	// integrated along the ray, which is an approximation -- but the fog
	// amount already scales with how much mist is in the way, so the mist
	// glows near a beam and does not far from one, which is the whole read.
#ifdef SCENE_LIGHT_MASK
	// [LIGHTMASK] Pinned light: the mist a beam lights. Only ignite is added after it.
	if (uBeamParams.z > 0.0)
	{
		gFogSlabPinnedLight = BeamLightAt(fragPos) * uBeamParams.z;
		col += gFogSlabPinnedLight;
	}
#else
	if (uBeamParams.z > 0.0)
		col += BeamLightAt(fragPos) * uBeamParams.z;
#endif

	// IGNITED MIST, last. It is light rather than density, so it is added to
	// the colour and it also forces a little coverage of its own -- a burning
	// cloud has to be visible in air that had no mist in it, or an explosion
	// in a clear room does nothing at all.
	if (ignite.r + ignite.g + ignite.b > 0.0)
	{
		col += ignite;
		amount = clamp(amount + min(ignite.r + ignite.g + ignite.b, 1.0) * 0.5, 0.0, 1.0);
	}

	return vec4(col, amount);
}

//
// [BB] WHAT IS DRAWN INSIDE A BAND.
//
// A band knows two things about every pixel it covers: how strongly it covers
// it, and where that pixel is in the world. It used to discard the second and
// blend one flat colour weighted by the first, so a band could only ever be a
// wash.
//
// EVERY SHAPE THAT DEFINES A DISTANCE ALSO IMPLIES TWO TANGENT COORDINATES,
// and a pattern is a function of those two:
//
//   bar east/west   distance abs(x-ox)     pattern runs in (z, y)
//   bar north/south distance abs(z-oz)     pattern runs in (x, y)
//   rising          distance  y-oy         pattern runs in (x, z)
//   ring            distance length(xz-o)  pattern runs in (arc, y)
//   shell           distance length(xyz-o) pattern runs in (longitude, y)
//
// So one function draws a lattice standing in a corridor AND a cage on an
// expanding cylinder, with no per-shape casing beyond picking the two axes.
//
// SPACING 0 IN AN AXIS MEANS NO LINES IN THAT AXIS. That single rule collapses
// grid, slats and a lone tripwire into one mode with one number changed, which
// is why there is no separate enum for any of them.
//
// LINE WIDTH IS IN WORLD UNITS, not a fraction of the spacing. A fractional
// width makes the lattice coarsen with distance and shimmer under motion; a
// world width holds its real size and antialiases cleanly against fwidth.
//
//   uSweepFill     spacing U, spacing V, line width, softness
//   uSweepFill2    rotation (deg), drift, major every N, jitter
//   uSweepFill3    gradient amount, gradient axis, flicker, major boost
//   uSweepFillCol  rgb line colour, w gap fill
//
// Returns line coverage 0..1.
//
// [round2 C2] The body is SweepLineAxisAA, which lets the caller supply the
// antialias width and a wrap count. aa < 0 takes fwidth(coord) at the same
// point this always did, so every plane band draws exactly as before. A ring or
// shell passes a width that does not jump where its arc coordinate wraps, and
// wrapCount > 0 hashes the line index modulo it, so jitter, flicker and majors
// give a line the same look on both sides of the wrap (see SweepArcFrame).
float SweepLineAxisAA(float coord, float spacing, float width, float soft, float t, float aa, float wrapCount)
{
	if (spacing <= 0.0) return 0.0;

	float idx = floor(coord / spacing + 0.5);
	float key = (wrapCount > 0.0) ? mod(idx, wrapCount) : idx;

	// JITTER -- push each line off the lattice by a stable hash of its own
	// index, so it reads as a row of emitters rather than a printed texture.
	if (uSweepFill2.w > 0.0)
	{
		float h = fract(sin(key * 78.233) * 43758.5453);
		coord += (h - 0.5) * spacing * uSweepFill2.w;
		idx = floor(coord / spacing + 0.5);
		key = (wrapCount > 0.0) ? mod(idx, wrapCount) : idx;
	}

	// FLICKER -- individual lines dropping out and returning. Instantly reads
	// as failing equipment, and it is per LINE rather than per band, which is
	// what stops it looking like the whole thing is blinking.
	if (uSweepFill3.z > 0.0)
	{
		float h = fract(sin(key * 12.9898 + floor(t * 8.0) * 3.717) * 43758.5453);
		if (h < uSweepFill3.z) return 0.0;
	}

	// MAJOR LINES -- every Nth one wider. Graph-paper structure for one mod.
	float w = width;
	if (uSweepFill2.z >= 2.0 && mod(abs(key), uSweepFill2.z) < 0.5)
		w *= max(uSweepFill3.w, 1.0);

	// Distance to the nearest line, in world units.
	float d = abs(coord - idx * spacing);

	// Antialias against the screen-space derivative as well as the authored
	// softness, so a line a hundred units away is still one clean line rather
	// than a moire.
	float fw = fwidth(coord);
	float aaw = max((aa < 0.0) ? fw : aa, 0.0001);
	return 1.0 - smoothstep(w, w + soft + aaw, d);
}

float SweepLineAxis(float coord, float spacing, float width, float soft, float t)
{
	return SweepLineAxisAA(coord, spacing, width, soft, t, -1.0, 0.0);
}

// [round2 C2] bandRadius is the band's own radius (uSweepBands[sb].x), which a
// ring or shell snaps its arc spacing against. Plane bands ignore it.
float SweepFillAt(int fill, int shape, vec3 origin, float bandRadius)
{
	if (fill <= 0) return 1.0;   // no fill: the band is a wash, as before

	// Pick the band's two tangent axes.
	// [round2 A2] From SweepPlaneAxis, the same answer the air lattice uses, so
	// the painted grid and the grid in the air share their tangents by
	// construction rather than by two lists that happen to agree.
	int faxis;
	float faway;
	SweepPlaneAxis(shape, faxis, faway);
	vec2 uv;
	vec2 originUV;              // [round2 C3] the origin in the same two axes
	float spacingU = uSweepFill.x;
	float wrapU = 0.0;
	float aaU = -1.0;           // -1: SweepLineAxisAA takes fwidth, as before
	float aaV = -1.0;
	if (faxis == 0)       { uv = vec2(pixelpos.z, pixelpos.y); originUV = vec2(origin.z, origin.y); }
	else if (faxis == 2)  { uv = vec2(pixelpos.x, pixelpos.y); originUV = vec2(origin.x, origin.y); }
	else if (faxis == 1)  { uv = vec2(pixelpos.x, pixelpos.z); originUV = vec2(origin.x, origin.z); }
	else
	{
		// Ring and shell: arc length around the axis, and height. Arc length
		// rather than raw angle so the spacing stays constant in world units
		// as the band expands -- an angular grid would spread apart as it
		// grew, which is not a cage, it is a fan.
		// [round2 C2] SweepArcFrame, which also snaps the arc spacing to a
		// whole number of lines and hands back wrap-safe antialias widths, so
		// the -X ray no longer carries a solid seam line. Shared with the air.
		SweepArcFrame(pixelpos.xyz, origin, bandRadius, uv, spacingU, wrapU, aaU, aaV);
		originUV = vec2(0.0, origin.y);   // arc is measured from the +X ray already
	}

	// [round2 C3] THE GRADIENT'S OWN COORDINATE, measured from the band's
	// origin along the same tangent axes, and taken BEFORE drift and rotation.
	// It used to read the drifted, rotated uv in absolute world units, so the
	// fade depended on where on the map the band happened to be and slid with
	// the drift. Now 0.5 sits at the origin and the fade holds still.
	vec2 gradUV = uv - originUV;

	// DRIFT -- the pattern sliding within the band as the band travels. This
	// is what makes it read as projected rather than painted onto the band.
	float t = timer;
	uv.x += t * uSweepFill2.y;

	// ROTATION in the band's own plane. Animate it and you get a lattice
	// turning inside a wall of light that is itself moving down a corridor.
	if (uSweepFill2.x != 0.0)
	{
		float a = radians(uSweepFill2.x);
		float cs = cos(a), sn = sin(a);
		uv = vec2(uv.x * cs - uv.y * sn, uv.x * sn + uv.y * cs);
		SweepRotateAA(cs, sn, aaU, aaV);
	}

	float cov;
	if (fill == 4)
	{
		// [round2 C1] PICKETS ON THE SURFACE TOO. Fill 4 used to exist only in
		// the air, so the walls behind a picket wall drew a grid. Same bars as
		// the air: the room-snapped spacing for a plane, the circumference snap
		// for a ring or shell, and no vertical term.
		float across = (faxis < 0) ? spacingU : SweepPicketSpacing(faxis);
		cov = SweepLineAxisAA(uv.x, across, uSweepFill.z, uSweepFill.w, t, aaU, wrapU);
	}
	else
	{
		float lu = SweepLineAxisAA(uv.x, spacingU, uSweepFill.z, uSweepFill.w, t, aaU, wrapU);
		float lv = SweepLineAxisAA(uv.y, uSweepFill.y, uSweepFill.z, uSweepFill.w, t, aaV, 0.0);
		cov = max(lu, lv);

		// DOTS -- fill 2 keeps only where the two axes CROSS, so the lattice
		// becomes a field of points. Same maths, one operator changed.
		if (fill == 2) cov = min(lu, lv);
	}

	// GRADIENT along one axis, so the lattice can be hot at floor level and
	// fade out overhead. Cheap, and it stops a grid looking like a decal.
	if (uSweepFill3.x > 0.0)
	{
		float g = (uSweepFill3.y > 0.5) ? gradUV.x : gradUV.y;
		g = clamp(g * 0.002 + 0.5, 0.0, 1.0);
		cov *= mix(1.0, g, uSweepFill3.x);
	}

	return clamp(cov, 0.0, 1.0);
}

vec4 getLightColor(Material material, float fogdist, float fogfactor)
{
	vec4 color = vColor;
#ifndef SHADER_LITE
	if (uLightLevel >= 0.0)
	{
		float newlightlevel = 1.0 - R_DoomLightingEquation(uLightLevel);
		color.rgb *= newlightlevel;
	}
	else if (uFogEnabled > 0)
	{
		// brightening around the player for light mode 2
		if (fogdist < uLightDist)
		{
			color.rgb *= uLightFactor - (fogdist / uLightDist) * (uLightFactor - 1.0);
		}

		//
		// apply light diminishing through fog equation
		//
		color.rgb = mix(vec3(0.0, 0.0, 0.0), color.rgb, fogfactor);
	}

	//
	// [BB] DARKNESS, AND IT GOES HERE FOR A REASON.
	//
	// AFTER the lighting equation, so it scales the light the room actually
	// ended up with -- including whatever Doom's own blinking and flickering
	// sectors did to it this tic, which is a thing the per-sector version had
	// to fight for and gets here for free.
	//
	// BEFORE the glow and the sweep, which is the important half. Those are
	// EMISSIVE: they are light this mod is adding, not light the room has.
	// Darkening them would darken the only thing left to see in a black room,
	// and the whole design is that the glow survives the dark. Everything
	// below this line is added on top of an already-darkened surface, exactly
	// as a light in a dark room is.
	//
	// uLightLevel is the sector's light, 0..1, and is what the curves want.
	// The fog path has no scalar light -- the colour IS the light there -- so
	// its luminance stands in, which keeps the two paths agreeing about how
	// dark a room is instead of one of them quietly opting out.
	// NOT IN 2D. uFogEnabled == -3 is the engine's existing marker for the
	// special 2D 'fog' mode (gl_renderstate.cpp:99, hw_draw2d.cpp:146), which
	// is how every HUD, menu and console quad reaches this shader.
	//
	// Without this the luminance fallback below catches them: 2D never sets a
	// light level, so it arrives with the same -1 sentinel the fog path uses,
	// and every HUD element gets darkened by its OWN brightness. The fallback
	// is right for fog, where the colour genuinely is the light. It is wrong
	// for a status bar, which was never in the world to be dark.
	if (uDarkness.x > 0.0 && uFogEnabled != -3 && uDarknessExempt < 1.0)
	{
		float dl = (uLightLevel >= 0.0) ? uLightLevel : grayscale(vec4(color.rgb, 1.0));
		// Partial, not all-or-nothing. uDarknessExempt is how much of the
		// darkening this draw is spared -- 0 takes all of it, 1 takes none,
		// and actors sit somewhere in between so a blacked-out room still has
		// things visible moving in it.
		color.rgb *= mix(DarknessAt(dl), 1.0, clamp(uDarknessExempt, 0.0, 1.0));
	}

	// [round2 B2] THE PASSED REGION, graded right after darkness and for the
	// same reason: it changes the light the room HAS, so the glow, the bands,
	// beams and stamps added below stay untouched. Same 2D gate as darkness.
	//
	// Draws fully exempt from darkness (uDarknessExempt 1: fullbright sprites,
	// UI billboards) are spared too, because they are not lit by the room. A
	// PARTIAL darkness exemption is not applied here: that is RS_Darkness's actor
	// setting, and a passed room grades what stands in it along with the room.
	if (uSweepPassedColor.w > 0.0 && uSweepCount > 0 && uFogEnabled != -3 && uDarknessExempt < 1.0)
	{
		float passed = SweepPassedAt();
		if (passed > 0.0)
		{
			vec3 graded = color.rgb * (1.0 - clamp(uSweepPassed.y, 0.0, 1.0));
			graded = mix(graded, vec3(grayscale(vec4(graded, 1.0))), clamp(uSweepPassed.z, 0.0, 1.0));
			graded = mix(graded, graded * uSweepPassedColor.rgb, clamp(uSweepPassed.x, 0.0, 1.0));
			color.rgb = mix(color.rgb, graded, passed);
		}
	}

	//
	// [BB] How strongly band sb covers THIS pixel, 0 if it does not.
	//
	// Factored out because two passes need it and they are separated by the
	// whole glow block: the recolour pass below has to run BEFORE the glow is
	// drawn (it changes what colour the glow is), and the light pass has to
	// run after (it changes what the finished pixel becomes). One copy of the
	// maths, so the two can never drift apart and disagree about where a band
	// is.
	//

	//
	// [BB] SWEEP RECOLOUR -- runs BEFORE the glow, because it changes what
	// colour the glow IS rather than adding light on top of it.
	//
	// Glow already varies per pixel VERTICALLY: glowdist is the fragment's
	// distance from the floor or ceiling plane, which is what makes coverage
	// and falloff smooth up a wall. Horizontally it could not vary at all,
	// because the colour arrives as one value for the whole surface. So a
	// wall could fade top to bottom beautifully and was a single flat colour
	// left to right.
	//
	// A band in recolour mode (4) blends the glow toward its own colour by
	// how strongly it covers this pixel. The result is a colour change that
	// SWEEPS ACROSS the wall instead of the room flipping: glow ahead of the
	// band is your palette, glow inside it is the band's, and the boundary
	// travels.
	//
	// Strongest band wins rather than accumulating -- two overlapping
	// recolours should hand over, not average into mud.
	//
	vec3 sweepTint = vec3(0.0);
	float sweepTintW = 0.0;
	for (int rb = 0; rb < 8; rb++)
	{
		if (rb >= uSweepCount) break;
		// MASK, because .w is a PACKED WORD -- drawmode + 16 * fill, the same
		// pair every other decode site here splits with & 15. Comparing the
		// whole word against 4 meant a recolour band stopped recolouring the
		// instant it was given any fill, since mode 4 with fill 1 packs as 20.
		// Two features that each worked alone and silently cancelled together.
		if ((int(uSweepBands[rb].w) & 15) != 4) continue;
		float ra = SweepBandAttenAt(rb) * uSweepColors[rb].a;
		if (ra > sweepTintW) { sweepTintW = ra; sweepTint = uSweepColors[rb].rgb; }
	}
	sweepTintW = clamp(sweepTintW, 0.0, 1.0);

	//
	// handle glowing walls
	//
	// [BB] Each glow can carry a SECOND colour. Without one a glow holds a
	// single colour and only dims as it fades, so a wall and the floor it
	// meets arrive at their shared line as two different colours at full
	// strength -- a hard edge no amount of falloff tuning removes. With a far
	// colour the glow ramps instead: the primary colour sits AT the plane
	// (atten 1) and the far colour is where it fades to (atten 0). Hand the
	// same junction colour to both surfaces and the corner becomes one
	// continuous ramp -- floor colour, blend, wall colour -- with no flat
	// region in it. Alpha 0 on the far colour means unset, and the glow is
	// byte-for-byte the flat wash it always was.
	//
	// [BB] The wave, per channel. Reach is modulated BEFORE the cutoff test,
	// which is the whole point: multiplying the finished contribution only
	// makes a straight-edged band pulse brighter and dimmer, while moving the
	// reach moves the edge itself. The far-colour ramp rides atten, so the
	// corner gradient stretches and squashes with the edge for free.
	//
	// Seeded from the glow plane's height, which is already here, already
	// per sector, and already differs between rooms.
	float wTop = GlowWaveRaw(uGlowWavePhase.x, GlowWaveSeedOff(uGlowTopPlane.w));
	float wBot = GlowWaveRaw(uGlowWavePhase.y, GlowWaveSeedOff(uGlowBottomPlane.w));

	// [BB] And the texture INSIDE the glow, which is where a lane goes once
	// its reach is high enough that the edge the wave moves is off screen.
	// Applied to the finished contribution rather than to reach, so it can
	// never move a band's shape -- the wave owns shape, this owns substance.
	float gTexTop = GlowTextureAt(GlowWaveSeedOff(uGlowTopPlane.w));
	float gTexBot = GlowTextureAt(GlowWaveSeedOff(uGlowBottomPlane.w));

	float topReach = uGlowTopColor.a * (1.0 + uGlowWaveDepth.x * wTop);
	if (uGlowTopColor.a > 0.0 && glowdist.x < topReach)
	{
		float topfrac = glowdist.x / topReach;
		float topatten;
		if (uGlowTopFalloff == 0)      topatten = 1.0 - topfrac;
		else if (uGlowTopFalloff == 1) topatten = 1.0 - topfrac * topfrac;
		else if (uGlowTopFalloff == 2) topatten = 1.0 - sqrt(topfrac);
		else                            topatten = exp(-topfrac * 3.0);
		vec3 gtop = uGlowTopColor.rgb;
		// Colour depth slides the near/far boundary without touching the
		// shape -- the band stands still and the colour moves through it.
		if (uGlowTopFar.a > 0.0)
			gtop = mix(uGlowTopFar.rgb, gtop, clamp(topatten + uGlowWaveDepth.z * wTop, 0.0, 1.0));
		gtop = mix(gtop, sweepTint, sweepTintW);
		color.rgb += desaturate(vec4(gtop * topatten * uGlowTopIntensity
			* (1.0 + uGlowWaveDepth.y * wTop) * gTexTop, 1.0)).rgb;
	}
	float botReach = uGlowBottomColor.a * (1.0 + uGlowWaveDepth.x * wBot);
	if (uGlowBottomColor.a > 0.0 && glowdist.y < botReach)
	{
		float botfrac = glowdist.y / botReach;
		float botatten;
		if (uGlowBottomFalloff == 0)      botatten = 1.0 - botfrac;
		else if (uGlowBottomFalloff == 1) botatten = 1.0 - botfrac * botfrac;
		else if (uGlowBottomFalloff == 2) botatten = 1.0 - sqrt(botfrac);
		else                                botatten = exp(-botfrac * 3.0);
		vec3 gbot = uGlowBottomColor.rgb;
		if (uGlowBottomFar.a > 0.0)
			gbot = mix(uGlowBottomFar.rgb, gbot, clamp(botatten + uGlowWaveDepth.z * wBot, 0.0, 1.0));
		gbot = mix(gbot, sweepTint, sweepTintW);
		color.rgb += desaturate(vec4(gbot * botatten * uGlowBottomIntensity
			* (1.0 + uGlowWaveDepth.y * wBot) * gTexBot, 1.0)).rgb;
	}

	//
	// [BB] flat-edge glow: floors/ceilings glow inward from their own
	// linedef edges. Unlike the wall glow above, this tints the flat's OWN
	// pixels, not a nearby wall's.
	//
	if (uFlatGlowColor.a > 0.0 && uFlatGlowLineCount > 0)
	{
		// [PERF] ONE SQUARE ROOT, NOT SIXTY-FOUR.
		//
		// This loop runs on EVERY floor and ceiling fragment in the game --
		// both flat glows default on, so uFlatGlowColor.a > 0 is the normal
		// state, and floors and ceilings are most of the screen. It was doing
		// a length() per segment: up to 64 sqrt and 64 divides per fragment,
		// which made it the largest per-fragment cost this fork adds.
		//
		// minDist is only ever consumed as `minDist < reach` and
		// `minDist / reach` below. Both comparisons order identically on
		// SQUARED values, so the whole search runs squared and takes a single
		// sqrt at the end. Bit-for-bit the same picture, 63 fewer roots.
		//
		// The bounding-box reject in front of each solve is the other half:
		// four compares against ~15 ALU plus a divide, and in a large sector
		// most linedefs are nowhere near the fragment.
		vec2 pixXZ = pixelpos.xz;
		float minDistSq = 999999.0 * 999999.0;
		// The real `reach` is computed further down, because it rides a glow
		// wave that has not been evaluated yet. So the reject uses the
		// LARGEST reach that wave could ever produce -- abs() on the depth so
		// the bound holds whichever way the wave swings, plus a unit of
		// slack. Conservative on purpose: rejecting too little only costs
		// speed, while rejecting too much would drop a glow that belongs.
		float rejectR = uFlatGlowColor.a * (1.0 + abs(uGlowWaveDepth.x)) + 1.0;

		for (int i = 0; i < uFlatGlowLineCount; i++)
		{
			vec4 seg = uFlatGlowLines[i];
			vec2 a = seg.xy;
			vec2 b = seg.zw;

			// Cheap reject: the segment's own XZ box, grown by the reach.
			vec2 lo = min(a, b) - rejectR;
			vec2 hi = max(a, b) + rejectR;
			if (pixXZ.x < lo.x || pixXZ.x > hi.x ||
			    pixXZ.y < lo.y || pixXZ.y > hi.y) continue;

			vec2 ab = b - a;
			vec2 ap = pixXZ - a;
			float t = clamp(dot(ap, ab) / max(dot(ab, ab), 0.001), 0.0, 1.0);
			vec2 d2 = ap - ab * t;
			minDistSq = min(minDistSq, dot(d2, d2));
		}

		float minDist = sqrt(minDistSq);

		// [BB] Floor and ceiling TIME-SHARE this uniform, because a draw only
		// ever covers one of them -- so the shader cannot tell which it is
		// and has to be told. uFlatGlowIsCeiling is what used to be a pad.
		// Without it both flats would take the same phase and the wave could
		// not climb a room.
		//
		// Seeded off the first linedef endpoint, which is per sector and
		// already uploaded for the distance search above.
		float flatPhase = (uFlatGlowIsCeiling != 0) ? uGlowWavePhase.w : uGlowWavePhase.z;
		float wFlat = GlowWaveRaw(flatPhase,
			GlowWaveSeedOff(uFlatGlowLines[0].x + uFlatGlowLines[0].y));

		float reach = uFlatGlowColor.a * (1.0 + uGlowWaveDepth.x * wFlat);
		if (minDist < reach)
		{
			float frac = minDist / reach;
			float atten;
			if (uFlatGlowFalloff == 0)      atten = 1.0 - frac;
			else if (uFlatGlowFalloff == 1) atten = 1.0 - frac * frac;
			else if (uFlatGlowFalloff == 2) atten = 1.0 - sqrt(frac);
			else                             atten = exp(-frac * 3.0);

			vec3 gflat = uFlatGlowColor.rgb;
			if (uFlatGlowFar.a > 0.0)
				gflat = mix(uFlatGlowFar.rgb, gflat, clamp(atten + uGlowWaveDepth.z * wFlat, 0.0, 1.0));
			// [BB] RECOLOUR REACHED TWO CHANNELS OUT OF FOUR. Both wall glows
			// mix sweepTint above and this one never did, so a recolour band
			// sweeping a room changed the walls and left the floor and the
			// ceiling on the old palette -- visible as the band crossing a
			// corner and stopping dead at it.
			gflat = mix(gflat, sweepTint, sweepTintW);
			// Same texture the walls get, seeded the same way, so a pattern
			// crossing a wall/floor join does not restart at the corner.
			color.rgb += desaturate(vec4(gflat * atten
				* (1.0 + uGlowWaveDepth.y * wFlat)
				* GlowTextureAt(GlowWaveSeedOff(uFlatGlowLines[0].x + uFlatGlowLines[0].y)),
				1.0)).rgb;
		}
	}

	//
	// [BB] sweep: a thin band of light at a fixed distance from an origin,
	// measured in world space. Because the distance is world-space and this
	// runs on every surface, the band wraps across floor, wall and ceiling
	// by itself -- a cylinder cuts all three at the same radius, a plane
	// draws an unbroken rectangle around a corridor. Nothing here knows or
	// cares what kind of surface it is shading, which is the entire trick.
	//
	if (uSweepCount > 0)
	{
		// EVERY BAND CARRIES ITS OWN ORIGIN AND ITS OWN SHAPE.
		//
		// This used to compute the distance ONCE, outside the loop, from a
		// single shared origin -- which made a train of eight nearly free but
		// forced all eight to be concentric and the same shape. That was a
		// shortcut, not a limit: nothing about a band requires it to agree
		// with its neighbours about where the centre of the world is.
		//
		// Now the distance moves inside the loop and reads uSweepBandOrigin
		// per band -- xyz is that band's origin, w is its shape. So a ring can
		// expand from the map centre while a column climbs out of a corner and
		// a plane sweeps the long axis, all in the same frame. The cost is eight
		// distance evaluations instead of one, which is a handful of ALU in a
		// shader that is already sampling textures.
		//
		// w <= 0 means "this band is off", which is also the gate that used to
		// live on the shared origin.
		for (int sb = 0; sb < 8; sb++)
		{
			if (sb >= uSweepCount) break;
			vec4 sband = uSweepBands[sb];

			// [BB] The draw mode and the FILL mode share this component --
			// drawmode + 16 * fill. See SetSweepBandDraw for why: draw mode
			// is 0-4 and always will be, so the rest of the float was free,
			// and a vec4[8] of per-band fill in StreamData would have cost
			// draw batching in every frame of the game.
			int bandpack = int(sband.w);
			int bmode = bandpack & 15;
			// [round2 B2] Masked: bit 8 (+256) is the passed-region flag, so
			// an unmasked shift would read passed + fill as fill 16+.
			int bfill = (bandpack >> 4) & 15;

			if (bmode <= 0) continue;
			// Recolour bands already had their say, above the glow.
			if (bmode == 4) continue;

			float satten = SweepBandAttenAt(sb);
			if (satten <= 0.0) continue;
			vec4 scol = uSweepColors[sb];

			// WHAT IS INSIDE THE BAND.
			//
			// The band's own colour is the FIELD and the fill colour is the
			// LINES. Gap 0 means only the lines are lit and the room shows
			// through between them, which is what reads as actual lasers --
			// turn it up and it becomes a lit pane with structure in it,
			// which is a completely different object.
			//
			// A negative gap inverts: lit gaps, dark lines. A grid of shadow.
			if (bfill > 0)
			{
				// [round2 C2] + the band radius, for the ring/shell arc snap.
				float cov = SweepFillAt(bfill, int(uSweepBandOrigin[sb].w), uSweepBandOrigin[sb].xyz, sband.x);

				// Fill 3 is SOLID -- the band ignores its own falloff and
				// becomes a flat slab of light with hard edges. A wall
				// rather than a glow, and the only fill that wants no lines.
				if (bfill == 3) { cov = 1.0; satten = satten > 0.0 ? 1.0 : 0.0; }

				float gap = uSweepFillCol.w;
				scol.rgb = mix(scol.rgb * max(gap, 0.0), uSweepFillCol.rgb, cov);
				if (gap < 0.0) scol.rgb = mix(uSweepFillCol.rgb, scol.rgb * (-gap), cov);
				satten *= max(cov, max(gap, 0.0));
			}

			// The wake. A band is symmetric until uSweepTrail says otherwise,
			// and then it is simply WIDER on the side it came from -- one
			// falloff stretched, not a second gradient bolted alongside, so
			// there is no seam at the core. That lives in SweepBandAttenAt
			// now, shared with the recolour pass above.
			//
			// WHAT THE BAND DOES TO THE PIXEL. sband.w was a bare on/off flag
			// hardcoded to 1.0 -- four bytes of nothing -- and is the mode
			// now, so this cost no extra uniform space. 0 still means off.
			//
			// ADD (1)    emits light additively. A glowing line, and useless
			//            as a reveal: adding a colour to a room crushed
			//            toward black gives you that colour, not the room.
			// LIFT (2)   multiplies what is already there. Darkness scales a
			//            sector's colour down, so the detail is not gone, it
			//            is small -- scaling back up restores it. Per pixel,
			//            so a room ten times the band's width reveals in a
			//            moving strip instead of switching on whole.
			// CRUSH (3)  the same operation inverted: a travelling darkness.
			// RECOLOUR (4) handled above, before the glow.
			if (bmode == 2)
			{
				color.rgb *= (1.0 + satten * scol.a);
#ifdef SCENE_LIGHT_MASK
				LightMaskScaleLight(vec3(1.0 + satten * scol.a));	// [LIGHTMASK]
#endif
			}
			else if (bmode == 3)
			{
				color.rgb *= max(0.0, 1.0 - satten * scol.a);
#ifdef SCENE_LIGHT_MASK
				LightMaskScaleLight(vec3(max(0.0, 1.0 - satten * scol.a)));	// [LIGHTMASK]
#endif
			}
			else
			{
				color.rgb += desaturate(vec4(scol.rgb * satten * scol.a, 1.0)).rgb;
			}
		}

	}

	// [BB] BEAMS. Additive, and here for the same reason the glow is here:
	// they are EMISSIVE. The darkness term ran further up, before all of
	// this, precisely so that light this mod adds survives being in a dark
	// room -- a laser that got dimmer as the room got darker would be a
	// contradiction.
	//
	// Surfaces near a beam brighten because they are near it. Nothing else
	// had to be spawned to make that happen.
#ifdef SCENE_LIGHT_MASK
	// [LIGHTMASK] Pinned light: the beam's light on this surface.
	vec3 beamSurfaceLight = BeamLightAt(pixelpos.xyz);
	color.rgb += beamSurfaceLight;
	gMaskPinnedLight += beamSurfaceLight;
#else
	color.rgb += BeamLightAt(pixelpos.xyz);
#endif

	// [STAMP] Surface stamps. Emissive, so they land here with the beams and
	// the glow lanes -- AFTER the lighting equation and after DarknessAt, which
	// is what lets a shot light up a room darkened to black. Takes the surface
	// normal because a shape that spins or tiles needs to know which way is
	// round on the surface it is being painted on.
	ApplySurfaceStamps(color.rgb, pixelpos.xyz, vWorldNormal.xyz);

	// [OUTLINE] The traced edge, emissive for the same reason the stamps are:
	// after the lighting equation and after DarknessAt, so a corpse stays lit
	// in a room turned black. Zero unless this draw is an outlined sprite.
	color.rgb += gOutlineEmissive;
#endif
#ifdef SCENE_LIGHT_MASK
	LightMaskScaleLight(1.0 / max(color.rgb, vec3(1.0)));	// [LIGHTMASK] the clamp below, per channel
#endif
	color = min(color, 1.0);

	// these cannot be safely applied by the legacy format where the implementation cannot guarantee that the values are set.
#if !defined LEGACY_USER_SHADER && !defined NO_LAYERS
	//
	// apply glow
	//
	color.rgb = mix(color.rgb, material.Glow.rgb, material.Glow.a);
#ifdef SCENE_LIGHT_MASK
	// [LIGHTMASK] The glow texture covers the light it mixes over (its own colour is emissive: E4a).
	LightMaskScaleLight(vec3(1.0 - material.Glow.a));
#endif

	//
	// apply brightmaps
	//
#ifdef SCENE_LIGHT_MASK
	// [LIGHTMASK] The clamp below, per channel (the brightmap's own light is emissive: E4a).
	LightMaskScaleLight(1.0 / max(color.rgb + material.Bright.rgb, vec3(1.0)));
#endif
	color.rgb = min(color.rgb + material.Bright.rgb, 1.0);
#endif

	//
	// apply other light manipulation by custom shaders, default is a NOP.
	//
#ifdef SCENE_LIGHT_MASK
	vec3 lightBeforeProcess = color.rgb;	// [LIGHTMASK]
#endif
	color = ProcessLight(material, color);
#ifdef SCENE_LIGHT_MASK
	LightMaskRescaleLight(lightBeforeProcess, color.rgb);
#endif
	
	//
	// apply lightmaps
	//
	if (vLightmap.z >= 0.0)
	{
		color.rgb += texture(LightMap, vLightmap).rgb;
	}

#ifdef EFFECT_LIGHTS
	// [EFFECTLIGHTS] Effect light, added like the dynamic lights below and sharing their clamp (hw_effectlightbuffer.h).
	if (uEffectLightMode != 0)
		color.rgb += desaturate(vec4(EffectLightsAt(pixelpos.xyz, material.Normal, uEffectLightMode), 1.0)).rgb;
#endif

	//
	// apply dynamic lights
	//
#ifdef SCENE_LIGHT_MASK
	// [LIGHTMASK] The lightmap above added light of neither class: the classes' light is unchanged by it.
	vec3 litColour = ProcessMaterialLight(material, color.rgb);
	LightMaskToSurface(material.Base.rgb, litColour);
	return vec4(litColour, material.Base.a * vColor.a);
#else
	return vec4(ProcessMaterialLight(material, color.rgb), material.Base.a * vColor.a);
#endif
}

//===========================================================================
//
// Applies colored fog
//
//===========================================================================

vec4 applyFog(vec4 frag, float fogfactor)
{
	return vec4(mix(uFogColor.rgb, frag.rgb, fogfactor), frag.a);
}

//===========================================================================
//
// The color of the fragment if it is fully occluded by ambient lighting
//
//===========================================================================

vec3 AmbientOcclusionColor()
{
	float fogdist;
	float fogfactor;

	//
	// calculate fog factor
	//
	if (uFogEnabled == -1)
	{
		fogdist = max(16.0, pixelpos.w);
	}
	else
	{
		fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
	}
	if (uThickFogDistance > 0.0)
	{
		if (fogdist > uThickFogDistance)
		{
			fogdist = fogdist + uThickFogMultiplier * (fogdist - uThickFogDistance);
		}
	}
	fogfactor = exp2 (uFogDensity * fogdist);

	return mix(uFogColor.rgb, vec3(0.0), fogfactor);
}

vec4 ApplyFadeColor(vec4 frag)
{
	if (uGlobalFade == 1 && uFogEnabled != 0)
	{
		float fogdist;
		if (uFogEnabled == 1 || uFogEnabled == -1) 
		{
			// standard fog (1 or -1)
			fogdist = max(16.0, pixelpos.w);
		}
		else 
		{
			// radial fog (2 or -2)
			fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
		}
		float visibility = exp(-pow((fogdist * uGlobalFadeDensity), uGlobalFadeGradient));
		visibility = clamp(visibility, 0.0, 1.0);
		vec4 fogcolor = uGlobalFadeColor;
		if (uGlobalFadeMode == -1)
		{
			frag = vec4(mix(fogcolor.rgb, frag.rgb, visibility), frag.a * visibility);
#ifdef SCENE_LIGHT_MASK
			LightMaskScale(visibility);	// [LIGHTMASK]
#endif
		}
		else if (uGlobalFadeMode == 2)
		{
			frag = vec4(fogcolor.rgb, frag.a) * visibility;
#ifdef SCENE_LIGHT_MASK
			LightMaskScale(0.0);	// [LIGHTMASK] the fade colour replaces the colour
#endif
		}
	}
	return frag;
}

#ifdef SURFACE_DAMAGE
//===========================================================================
//
// [SURFACEDAMAGE] LASTING SURFACE DAMAGE ("Engine docs/SURFACE_DAMAGE_PLAN.md" #17,
// "Engine docs/SURFACE_DAMAGE_17_IMPL_NOTES.md"): bullet holes, gouges, soot, scorch, hot
// metal and wet patches that stay on walls and floors. Only the Vulkan backend's programs
// of this lump define SURFACE_DAMAGE (vk_shader.cpp); GL and GLES never compile any of it.
//
// A wall part or a flat that owns damage tiles is drawn with its record slot + 1 in the
// per-draw data; every other draw -- and every draw while nothing is damaged -- has 0 and
// returns on the first line of ApplySurfaceDamage. The record gives the surface's own
// (u, v) in map units as two planes over the world position: for a wall, u along its line
// and v down from its pegging anchor, so holes ride a door's face with the door. The hash
// finds the 64-unit cell's tile; the tile's texels are r soot, g hole depth, b heat,
// a wetness (hw_surfacedamageframe.h has the layouts and the C++ twin of the hash).
//
// It changes the material before anything lights it, and adds the heat glow as emissive
// light after getLightColor. No beam, fog slab or viewpoint value is read or changed here.
//
//===========================================================================

// The per-draw key: uFogPad2's slot, which hw_renderstate.h names uSurfaceDamageKey.
#define uSurfaceDamageKey data[uDataIndex].uFogPad2

// hw_surfacedamageframe.h: [0] scales (soot, depth, heat, show tiles), [1] x the hash's
// entry count, [2..) 16 looks x 4 vec4 (rim, inside, detail, finish), [66..) 8,192 surface
// records x 2 vec4 (U, V), [16450..) the hash (slot + 1, cell u, cell v, tile + look x 65536).
layout(set = 1, binding = 13, std430) buffer readonly SurfaceDamageSSO
{
	vec4 surfaceDamageData[];
};
layout(set = 0, binding = 7) uniform sampler2DArray SurfaceDamagePages;
layout(set = 0, binding = 8) uniform sampler2DArray SurfaceDamageDetail;

const int SURFACE_DAMAGE_LOOKS_AT = 2;
const int SURFACE_DAMAGE_SURFACES_AT = 66;
const int SURFACE_DAMAGE_HASH_AT = 16450;

// The heat glow ApplySurfaceDamage found for this pixel, added after the lighting in main().
bool gSurfaceDamageGlows = false;
vec3 gSurfaceDamageGlow = vec3(0.0);

// hw_surfacedamageframe.h SurfaceDamageHash, token for token.
uint SurfaceDamageHash(uint slot, int cellU, int cellV)
{
	uint h = slot * 0x9E3779B1u;
	h ^= uint(cellU) * 0x85EBCA77u;
	h = (h ^ (h >> 15)) * 0x2C1B3C6Du;
	h ^= uint(cellV) * 0xC2B2AE3Du;
	h = (h ^ (h >> 12)) * 0x297A2D39u;
	return h ^ (h >> 15);
}

// Hot metal: dull red, through orange, to yellow-white, brighter than 1 at full heat so bloom takes it.
vec3 SurfaceDamageHeatColor(float heat)
{
	vec3 dull = vec3(0.45, 0.04, 0.0);
	vec3 orange = vec3(1.0, 0.36, 0.05);
	vec3 whiteHot = vec3(1.0, 0.86, 0.6);
	vec3 ramp = heat < 0.5 ? mix(dull, orange, heat * 2.0) : mix(orange, whiteHot, heat * 2.0 - 1.0);
	return ramp * heat * heat * 3.0;
}

void ApplySurfaceDamage(inout Material material)
{
	int damageKey = uSurfaceDamageKey;
	if (damageKey <= 0)
		return;

	// From here the control flow is the same for the whole draw, so every derivative is taken here and every texture read
	// below has a sound LOD, whatever cell or tile a pixel is in.
	int record = SURFACE_DAMAGE_SURFACES_AT + (damageKey - 1) * 2;
	vec4 planeU = surfaceDamageData[record];
	vec4 planeV = surfaceDamageData[record + 1];
	vec2 uv = vec2(dot(pixelpos.xyz, planeU.xyz) + planeU.w, dot(pixelpos.xyz, planeV.xyz) + planeV.w);
	vec2 uvDx = dFdx(uv);
	vec2 uvDy = dFdy(uv);
	vec2 stDx = dFdx(vTexCoord.st);
	vec2 stDy = dFdy(vTexCoord.st);

	// The cell's tile: four probes of the hash. Not found leaves everything below at no damage.
	vec2 cell = floor(uv / 64.0);
	uint hashMask = uint(surfaceDamageData[1].x) - 1u;
	uint damageHash = SurfaceDamageHash(uint(damageKey - 1), int(cell.x), int(cell.y));
	float found = 0.0;
	int tileAndLook = 0;
	for (int probe = 0; probe < 4; probe++)
	{
		vec4 entry = surfaceDamageData[SURFACE_DAMAGE_HASH_AT + int((damageHash + uint(probe)) & hashMask)];
		if (entry.x == float(damageKey) && entry.y == cell.x && entry.z == cell.y)
		{
			tileAndLook = int(entry.w);
			found = 1.0;
			break;
		}
	}

	int tile = tileAndLook % 65536;
	int look = clamp(tileAndLook / 65536, 0, 15);
	int slot = tile % 225;
	vec2 texel = vec2(float(slot % 15), float(slot / 15)) * 132.0 + 2.0 + (uv - cell * 64.0) * 2.0;
	vec3 at = vec3(texel / 2048.0, float(tile / 225));
	float lod = clamp(log2(max(max(length(uvDx), length(uvDy)) * 2.0, 1.0)), 0.0, 1.0);
	vec4 damage = textureLod(SurfaceDamagePages, at, lod) * found;
	float depthAlongU = textureLod(SurfaceDamagePages, at + vec3(1.0 / 2048.0, 0.0, 0.0), lod).g * found;
	float depthAlongV = textureLod(SurfaceDamagePages, at + vec3(0.0, 1.0 / 2048.0, 0.0), lod).g * found;

	vec4 scales = surfaceDamageData[0];
	int lookAt = SURFACE_DAMAGE_LOOKS_AT + look * 4;
	vec4 lookRim = surfaceDamageData[lookAt];
	vec4 lookInside = surfaceDamageData[lookAt + 1];
	vec4 lookDetail = surfaceDamageData[lookAt + 2];
	vec4 lookFinish = surfaceDamageData[lookAt + 3];

	float soot = clamp(damage.r * scales.x, 0.0, 1.0);
	float depth = clamp(damage.g * scales.y, 0.0, 1.0);
	float heat = clamp(damage.b * scales.z, 0.0, 1.0);
	float wet = damage.a;
	// The depth's climb per texel along u and v: a hole's edge.
	vec2 slope = vec2(depthAlongU - damage.g, depthAlongV - damage.g) * scales.y;

	// The look's tiling detail breaks holes and rims up (explicit gradients: no LOD jump at a cell's edge).
	float detailScale = max(lookDetail.y, 1.0);
	vec4 grain = textureGrad(SurfaceDamageDetail, vec3(uv / detailScale, lookDetail.x), uvDx / detailScale, uvDy / detailScale);
	float hole = clamp(depth * mix(1.0, 0.55 + 0.9 * grain.g, lookDetail.z), 0.0, 1.0);
	float rim = clamp(length(slope) * lookDetail.w, 0.0, 1.0) * (1.0 - hole) * mix(1.0, 0.5 + grain.r, lookDetail.z);

	// Inside a hole the surface's own texture is seen further in: fetched again, shifted away from the eye by the look's depth,
	// through the (u, v) -> (s, t) map the derivatives give. With no hole the shift is 0 and the texel is the same one.
	mat2 uvMap = mat2(uvDx, uvDy);
	mat2 uvToSt = abs(determinant(uvMap)) > 1.0e-10 ? mat2(stDx, stDy) * inverse(uvMap) : mat2(0.0);
	vec3 toEye = uCameraPos.xyz - pixelpos.xyz;
	float eyeDistance = max(length(toEye), 1.0);
	float facing = max(abs(dot(toEye / eyeDistance, normalize(vWorldNormal.xyz))), 0.25);
	vec2 shift = -vec2(dot(toEye, planeU.xyz), dot(toEye, planeV.xyz)) / (eyeDistance * facing) * lookFinish.w * hole;
	vec4 inner = getTexel(vTexCoord.st + uvToSt * shift);

	vec3 base = material.Base.rgb;
	base = mix(base, inner.rgb, hole * step(0.001, lookFinish.w));
	base = mix(base, base * lookInside.rgb, hole * lookInside.a);
	base = mix(base, lookRim.rgb, rim * lookRim.a);
	base *= 1.0 - soot * (0.8 + 0.2 * grain.a);
	base *= 1.0 - 0.3 * wet;
	material.Base.rgb = base;

	// Holes bend the surface for dynamic lights (sector light does not use normals, so holes read mostly through the darkening
	// and the parallax above).
	vec3 bend = (slope.x * normalize(planeU.xyz) + slope.y * normalize(planeV.xyz)) * lookFinish.z * 2.0;
	if (dot(material.Normal, material.Normal) > 0.25 && dot(bend, bend) > 0.0)
		material.Normal = normalize(material.Normal + bend);

	// Wet: more specular and less rough (the SPECULAR and PBR materials light it).
	float shine = wet * lookFinish.x;
	material.Specular += vec3(shine);
	material.Glossiness = max(material.Glossiness, shine * 48.0);
	material.SpecularLevel += shine;
	material.Roughness = mix(material.Roughness, 0.08, clamp(shine, 0.0, 1.0));

	// Only hot things glow.
	if (heat > 0.0)
	{
		gSurfaceDamageGlow = SurfaceDamageHeatColor(heat) * (0.8 + 0.4 * grain.r);
		gSurfaceDamageGlows = true;
	}

	// "Show damage tiles": each tile tinted by its look, its edges drawn.
	if (scales.w > 0.5 && found > 0.5)
	{
		vec2 inTile = uv - cell * 64.0;
		float edge = step(min(min(inTile.x, inTile.y), min(64.0 - inTile.x, 64.0 - inTile.y)), 1.0);
		vec3 tint = fract(vec3(0.37, 0.61, 0.83) * float(look + 1)) * 0.6 + 0.4;
		material.Base.rgb = mix(material.Base.rgb * mix(vec3(1.0), tint, 0.35), vec3(1.0, 0.9, 0.2), edge);
	}
}
#endif

//===========================================================================
//
// Main shader routine
//
//===========================================================================

void main()
{
#ifdef NO_CLIPDISTANCE_SUPPORT
	if (ClipDistanceA.x < 0.0 || ClipDistanceA.y < 0.0 || ClipDistanceA.z < 0.0 || ClipDistanceA.w < 0.0 || ClipDistanceB.x < 0.0) discard;
#endif

#ifndef LEGACY_USER_SHADER
	Material material;

	material.Base = vec4(0.0);
	material.Bright = vec4(0.0);
	material.Glow = vec4(0.0);
	material.Normal = vec3(0.0);
	material.Specular = vec3(0.0);
	material.Glossiness = 0.0;
	material.SpecularLevel = 0.0;
	material.Metallic = 0.0;
	material.Roughness = 0.0;
	material.AO = 0.0;
	SetupMaterial(material);
#else
	Material material = ProcessMaterial();
#endif

#ifdef SURFACE_DAMAGE
	// [SURFACEDAMAGE] Lasting damage changes this surface's material before anything lights it (every draw without damage: one
	// compare and a return).
	ApplySurfaceDamage(material);
#endif

	// [OUTLINE] An actor traced in neon by its own sprite. HERE, before the
	// alpha test below, because the wire mode erases the body by rewriting
	// material.Base.a -- do it after the test and the body has already been
	// kept. Off for every draw that is not an outlined sprite, and off is one
	// float compare. The glowing half is left in gOutlineEmissive and spent in
	// getLightColor with the stamps.
	ApplySpriteOutline(material.Base, vTexCoord.st);
	vec4 frag = material.Base;

#ifndef NO_ALPHATEST
	if (frag.a <= uAlphaThreshold) discard;
#endif

	if (uFogEnabled != -3)	// check for special 2D 'fog' mode.
	{
		float fogdist = 0.0;
		float fogfactor = 0.0;
#ifdef SHADER_LITE
		fogdist = max(16.0, pixelpos.w);
		fogfactor = exp2 (uFogDensity * fogdist);
		frag = getLightColor(material, fogdist, fogfactor);
#else
		//
		// calculate fog factor
		//
		if (uFogEnabled != 0)
		{
			if (uFogEnabled == 1 || uFogEnabled == -1)
			{
				fogdist = max(16.0, pixelpos.w);
			}
			else
			{
				fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
			}
			if (uThickFogDistance > 0.0)
			{
				if (fogdist > uThickFogDistance)
				{
					fogdist = fogdist + uThickFogMultiplier * (fogdist - uThickFogDistance);
				}
			}
			fogfactor = exp2 (uFogDensity * fogdist);
		}

		if ((uTextureMode & 0xffff) != 7)
		{
			frag = getLightColor(material, fogdist, fogfactor);
#ifdef SURFACE_DAMAGE
			// [SURFACEDAMAGE] Hot metal glows: emissive light added after the lighting, so a dark room does not dim it and bloom
			// takes it; the fog below still covers it.
			if (gSurfaceDamageGlows)
			{
				frag.rgb += gSurfaceDamageGlow;
#ifdef SCENE_LIGHT_MASK
				gMaskEmissive += gSurfaceDamageGlow.r + gSurfaceDamageGlow.g + gSurfaceDamageGlow.b;	// [LIGHTMASK] emissive light
#endif
			}
#endif

			//
			// colored fog
			//
			if (uFogEnabled < 0)
			{
				frag = applyFog(frag, fogfactor);
#ifdef SCENE_LIGHT_MASK
				LightMaskScale(fogfactor);	// [LIGHTMASK] mix(fog, frag, fogfactor)
#endif
			}
		}
		else
		{
			frag = vec4(uFogColor.rgb, (1.0 - fogfactor) * frag.a * 0.75 * vColor.a);
		}
#endif
		frag = ApplyFadeColor(frag);

		// [BB] THE FOG SLAB GOES LAST, AND OVER EVERYTHING.
		//
		// This is the opposite placement to the darkness term, which runs
		// BEFORE the glow so that emissive light survives being in a dark
		// room. Fog is not darkness -- it is a substance sitting BETWEEN the
		// eye and the surface, so it occludes whatever is behind it including
		// the glow. A glowing floor seen through knee-deep mist should be a
		// glow diffused by mist, not a glow with mist politely behind it.
		vec4 slab = FogSlabAt(pixelpos.xyz);
		if (slab.a > 0.0)
		{
			// [BB] THE MIST PICKS UP WHAT IS BEHIND IT.
			//
			// Without this the slab is a flat colour laid over the scene, and
			// it reads as a filter rather than as a substance: mist standing
			// in front of a red glowing wall stays its own colour, which is
			// wrong in a way that is instantly obvious even if you cannot
			// name it. Real mist near a coloured light IS that colour.
			//
			// A true scattering integral would gather light along the ray.
			// This gathers it from the one place it is already known -- the
			// pixel behind the fog, which is the wall, its glow, any sweep
			// band crossing it, everything. So a red wall glow bleeds into
			// the mist in front of it for one mix, no extra sampling.
			//
			// At pickup 0 the fog keeps its own colour exactly as before.
			vec3 fogCol = mix(slab.rgb, slab.rgb * (0.35 + 0.65 * frag.rgb), uFogSlabExtra.y);
#ifdef SCENE_LIGHT_MASK
			// [LIGHTMASK] fogCol = slab x mix(1, 0.35 + 0.65 x frag, pickup) per channel, so the mist's
			// own beam light goes through that factor exactly (the colour it picks up from behind
			// counts as mist). What the mist covers keeps 1 - slab.a of its amounts.
			float mistPinned = max(dot(mix(vec3(1.0), 0.35 + 0.65 * frag.rgb, uFogSlabExtra.y), gFogSlabPinnedLight), 0.0);
			LightMaskScale(1.0 - slab.a);
			gMaskPinned += mistPinned * slab.a;
#endif
			frag.rgb = mix(frag.rgb, fogCol, slab.a);
		}

		// [BB] THE BEAM ITSELF, SEEN IN THE AIR.
		//
		// Last, and after the fog on purpose. Everything before this point
		// lights SURFACES; this is the beam as an object hanging in space,
		// and it should not be dimmed by mist it is in front of.
		//
		// It is also the last thing written before the frame is handed to
		// bloom, so a core burning past white blooms the way an emissive
		// thing should -- without a light, a sprite, or a quad.
		// [BB] And the sweep's own lattice, hanging in the air inside the band
		// rather than painted on what the band lands on.
		{
			float airOcc; vec3 airOccCol;
			frag.rgb += SweepAirLattice(pixelpos.xyz, airOcc, airOccCol);
			// Blended AFTER the additive term, so a band that occludes hides
			// what is behind it rather than being washed out by its own light.
			if (airOcc > 0.0) frag.rgb = mix(frag.rgb, airOccCol, airOcc);
#ifdef SCENE_LIGHT_MASK
			// [LIGHTMASK] What the lattice occludes loses its share (the lattice's light is emissive: E4a).
			if (airOcc > 0.0) LightMaskScale(1.0 - airOcc);
#endif
		}

		// [BB] Shapes drawn onto surfaces. Emissive, so they go here with the
		// rest of the light rather than through the lighting equation -- a
		// mark burned onto a floor does not get darker because the room is.
		//
		// Sprites carry no world normal, so they are skipped rather than
		// guessed at: a shape smeared across a monster standing in it would
		// read as a rendering fault, and the floor beneath is drawn anyway.
		if (dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5)
			frag.rgb += ShapesAt(pixelpos.xyz, normalize(vWorldNormal.xyz));

		// [BB] Standing shapes, unconditionally -- see StandingShapesAt()'s
		// own header for why this cannot live behind the normal check above.
		frag.rgb += StandingShapesAt(pixelpos.xyz);

#ifdef SCENE_LIGHT_MASK
		// [LIGHTMASK] Pinned light: the beam seen in the air.
		vec3 beamAirLight = BeamAirGlow(pixelpos.xyz);
		frag.rgb += beamAirLight;
		gMaskPinned += beamAirLight.r + beamAirLight.g + beamAirLight.b;
#else
		frag.rgb += BeamAirGlow(pixelpos.xyz);
#endif
	}
	else // simple 2D (uses the fog color to add a color overlay)
	{
		if ((uTextureMode & 0xffff) == 7)
		{
			float gray = grayscale(frag);
			vec4 cm = (uObjectColor + gray * (uAddColor - uObjectColor)) * 2.0;
			frag = vec4(clamp(cm.rgb, 0.0, 1.0), frag.a);
		}
			frag = frag * ProcessLight(material, vColor);
		frag.rgb = frag.rgb + uFogColor.rgb;
	}
	
	FragColor = frag;

#ifdef MODEL_EYE_FADE
	// [EYEFADE] A model near the eye dissolves instead of filling the view (EFF_EYEFADE;
	// VR_BODY_IK_RETURN_PLAN.md 4b idea 5). The share of pixels kept eases from all of them
	// at uEyeFadeFar to none at uEyeFadeNear (map units from this view's eye); the rest are
	// discarded against DITHERTRANS's 8x8 Bayer thresholds, in SCREEN cells here so the
	// pattern stays fine at arm's length. Kept pixels write depth like any solid model.
	// far <= near: nothing is discarded.
	if (uEyeFadeFar > uEyeFadeNear)
	{
		const float EYE_FADE_BAYER[64] =
		float[64](
			1.0 / 65.0, 33.0 / 65.0, 9.0 / 65.0, 41.0 / 65.0, 3.0 / 65.0, 35.0 / 65.0, 11.0 / 65.0, 43.0 / 65.0,
			49.0 / 65.0, 17.0 / 65.0, 57.0 / 65.0, 25.0 / 65.0, 51.0 / 65.0, 19.0 / 65.0, 59.0 / 65.0, 27.0 / 65.0,
			13.0 / 65.0, 45.0 / 65.0, 5.0 / 65.0, 37.0 / 65.0, 15.0 / 65.0, 47.0 / 65.0, 7.0 / 65.0, 39.0 / 65.0,
			61.0 / 65.0, 29.0 / 65.0, 53.0 / 65.0, 21.0 / 65.0, 63.0 / 65.0, 31.0 / 65.0, 55.0 / 65.0, 23.0 / 65.0,
			4.0 / 65.0, 36.0 / 65.0, 12.0 / 65.0, 44.0 / 65.0, 2.0 / 65.0, 34.0 / 65.0, 10.0 / 65.0, 42.0 / 65.0,
			52.0 / 65.0, 20.0 / 65.0, 60.0 / 65.0, 28.0 / 65.0, 50.0 / 65.0, 18.0 / 65.0, 58.0 / 65.0, 26.0 / 65.0,
			16.0 / 65.0, 48.0 / 65.0, 8.0 / 65.0, 40.0 / 65.0, 14.0 / 65.0, 46.0 / 65.0, 6.0 / 65.0, 38.0 / 65.0,
			64.0 / 65.0, 32.0 / 65.0, 56.0 / 65.0, 24.0 / 65.0, 62.0 / 65.0, 30.0 / 65.0, 54.0 / 65.0, 22.0 / 65.0
		);
		float eyeFadeKeep = smoothstep(uEyeFadeNear, uEyeFadeFar, distance(pixelpos.xyz, uCameraPos.xyz));
		int eyeFadeCell = (int(gl_FragCoord.x) % 8) * 8 + int(gl_FragCoord.y) % 8;
		if (EYE_FADE_BAYER[eyeFadeCell] >= eyeFadeKeep) discard;
	}
#endif
#ifdef DITHERTRANS
	int index = (int(pixelpos.x) % 8) * 8 + int(pixelpos.y) % 8;
	const float DITHER_THRESHOLDS[64] =
	float[64](
		1.0 / 65.0, 33.0 / 65.0, 9.0 / 65.0, 41.0 / 65.0, 3.0 / 65.0, 35.0 / 65.0, 11.0 / 65.0, 43.0 / 65.0,
		49.0 / 65.0, 17.0 / 65.0, 57.0 / 65.0, 25.0 / 65.0, 51.0 / 65.0, 19.0 / 65.0, 59.0 / 65.0, 27.0 / 65.0,
		13.0 / 65.0, 45.0 / 65.0, 5.0 / 65.0, 37.0 / 65.0, 15.0 / 65.0, 47.0 / 65.0, 7.0 / 65.0, 39.0 / 65.0,
		61.0 / 65.0, 29.0 / 65.0, 53.0 / 65.0, 21.0 / 65.0, 63.0 / 65.0, 31.0 / 65.0, 55.0 / 65.0, 23.0 / 65.0,
		4.0 / 65.0, 36.0 / 65.0, 12.0 / 65.0, 44.0 / 65.0, 2.0 / 65.0, 34.0 / 65.0, 10.0 / 65.0, 42.0 / 65.0,
		52.0 / 65.0, 20.0 / 65.0, 60.0 / 65.0, 28.0 / 65.0, 50.0 / 65.0, 18.0 / 65.0, 58.0 / 65.0, 26.0 / 65.0,
		16.0 / 65.0, 48.0 / 65.0, 8.0 / 65.0, 40.0 / 65.0, 14.0 / 65.0, 46.0 / 65.0, 6.0 / 65.0, 38.0 / 65.0,
		64.0 / 65.0, 32.0 / 65.0, 56.0 / 65.0, 24.0 / 65.0, 62.0 / 65.0, 30.0 / 65.0, 54.0 / 65.0, 22.0 /65.0
	);

	vec3 fragHSV = rgb2hsv(FragColor.rgb);
	float brightness = clamp(1.5*fragHSV.z, 0.1, 1.0);
	if (DITHER_THRESHOLDS[index] < brightness) discard;
	else FragColor *= 0.5;
#ifdef SCENE_LIGHT_MASK
	LightMaskScale(0.5);	// [LIGHTMASK]
#endif
#endif

#ifdef SCENE_LIGHT_MASK
	// [LIGHTMASK] With the colour's own alpha: the blend state is shared by every attachment.
	FragLightMask = vec4(gMaskEmissive, gMaskPinned, 0.0, FragColor.a);
#endif
#ifdef GBUFFER_PASS
	FragFog = vec4(AmbientOcclusionColor(), 1.0);
	FragNormal = vec4(vEyeNormal.xyz * 0.5 + 0.5, 1.0);
#endif
}
