/*
** drawnlines.fp
**
** [DRAWNLINES] The glow of ONE line, for each pixel of the box drawnlines.vp
** built around it. See hw_drawnlinebuffer.h.
**
** THE MATHS IS main.fp's BeamAirGlow, line for line, for a single line: the same
** ray reject, the same closest approach between the view ray and the segment,
** the same taper, scroll, flare, and the same two falloffs with the same
** constants. Given the same line and the same pixel, the glow value is the
** same number. What differs is everything AROUND that number, listed below.
**
** Drawn with STYLE_Add and alpha 1, so the glow is simply added to the frame,
** as main.fp adds it. Values past white are kept (the scene buffer is HDR), so it
** feeds bloom exactly as the per-pixel beam does.
**
** WHERE THE DRAWN LOOK CANNOT MATCH THE PER-PIXEL BEAM
**
**  1. It lights no surfaces. BeamLightAt -- walls brightening near a beam, and
**     the fog it crosses glowing -- needs every surface pixel to ask about every
**     line, which is exactly the cost this path exists to avoid. For beams
**     routed by r_beams_drawn, r_beams_drawn_surfacelight (on by default) keeps
**     that surface light per pixel; only the glow in the air moves here.
**
**  2. Occlusion is a depth test, not a per-pixel clamp. BeamAirGlow clamps the
**     closest approach to the distance of the surface under the pixel, so a
**     surface cuts the glow exactly and smoothly. This shader cannot see scene
**     depth. It writes its own depth at the closest approach, pulled toward the
**     eye by a bias (r_drawnlines_depthbias; automatic = the line's halo reach)
**     and lets the depth test decide:
**       - at an impact, the halo splashing onto the wall is continuous, as
**         per-pixel, because the bias keeps it in front of the wall;
**       - but anything standing in front of the line and closer to it than the
**         bias does not hide it: the line's full glow shows over that edge,
**         where per-pixel shows only the halo falling onto the occluder. For a
**         thin grab laser the bias is a few units; for the Lance's sheath at its
**         hottest it is tens.
**       - lower the bias and occluders cut sooner, but the impact halo gets a
**         visible edge where the box's depth meets the wall.
**
**  3. Translucent and additive surfaces. main.fp adds the glow on EVERY surface
**     layer it shades -- glass, water, translucent and additive sprites -- and
**     that layer's own alpha then scales it. Drawn lines are added once, after
**     the translucent pass, depth-tested against opaque depth only: over a
**     translucent sprite the line is at full strength, and it is not doubled on
**     additive effects the way the per-pixel glow is.
**
**  4. Weapon models held in VR. They draw before the translucent pass with
**     their depth squeezed into the front 30% of the range (BeginDrawHUDModel
**     in hw_models.cpp, the view-model trick that keeps a gun out of walls), so
**     they hide a drawn line wherever they cover it -- even where the line is
**     in front of them. The per-pixel glow is added onto the gun's surface
**     instead: halo around the Lance's muzzle that overlaps the gun shows
**     per-pixel, and is cut at the gun's silhouette when drawn.
**
**  5. Scroll timing. main.fp's `timer` carries the shader speed of the material
**     under the pixel; drawn lines use speed 1, which is the value for ordinary
**     walls and flats. Only matters with scroll depth above 0 (the grab lasers'
**     default is 0.25; the Lance uses 0).
**
**  6. Dither-translucent surfaces (DITHERTRANS) halve or discard the per-pixel
**     glow on their pixels; drawn lines ignore them.
**
**  7. Main view only. Portals, mirrors and skyboxes do not draw drawn lines,
**     and a beam routed by r_beams_drawn has no glow in the air there either.
**     Camera textures do draw them. GL and GLES never draw them, and there
**     r_beams_drawn changes nothing.
**
** [F1] TWO OPT-IN LOOKS ("Engine docs/FLAME_ENGINE_PLAN.md" F1), both off unless a
** script asked (SetDrawnLineGradient / SetDrawnLineTurbulence -> the record's look
** flags g.w, hw_drawinfo.cpp's WriteDrawnLineLooks). They have no per-pixel
** counterpart, so nothing above about matching BeamAirGlow applies to them. Every
** step of either sits behind its flag: with flags 0 -- every routed beam, every line
** that never asked -- this shader runs exactly the code it ran before they existed.
**
**   GRADIENT (1). The colour runs from the start colour (c.rgb) to the end colour
**   (f.rgb) along the line, and the halo's reach (soft) grows toward the far end by
**   mix(1, swell, tc): a jet that widens as it travels while its core stays thin.
**   Swell below 1 narrows it. A polyline gives each segment the colours of its own
**   two ends and the swell ratio between them, so the joins match.
**
**   TURBULENCE (2). Two octaves of 3D value noise (valuenoise.glsl, the volumetric
**   beam's dust noise and octave mix), sampled in WORLD space at the closest
**   approach on the view ray, g.y cells per map unit, rising at g.z cells per second
**   of LEVEL time (uLevelTime.x: it stops when the game pauses, as the particles
**   do). Centred and scaled by strength (g.x), it moves the pixel's distance from the
**   line by up to strength x that distance, capped at the local reach -- the core
**   hardly moves, the edges lick -- and its brightness by up to half the strength.
**   World space, so both eyes see the same licks at the same depth. Cost: sixteen
**   hashes per pixel that could glow; pixels past the widest lick are discarded
**   before the noise.
**
**   Both looks reach further, so the reject below, the automatic depth bias and
**   drawnlines.vp's box grow with them.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

layout(location = 0) in vec3 vLineWorld;
layout(location = 1) flat in vec4 vLineA;
layout(location = 2) flat in vec4 vLineB;
layout(location = 3) flat in vec4 vLineCol;
layout(location = 4) flat in vec4 vLineLook;
layout(location = 5) flat in vec4 vLineFX;
// [F1] The gradient look (rgb end colour, swell) and the turbulence look
// (strength, noise scale, rise speed, look flags) -- the record's f and g.
layout(location = 6) flat in vec4 vLineGradient;
layout(location = 7) flat in vec4 vLineTurbulence;

layout(location = 0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location = 1) out vec4 FragFog;
layout(location = 2) out vec4 FragNormal;
#endif
#ifdef SCENE_LIGHT_MASK
// [LIGHTMASK] The light mask (hw_postprocess.h, PPLightMask; only the Vulkan mask programs
// define SCENE_LIGHT_MASK, and the C++ sets LIGHT_MASK_LOCATION). This line's glow, r + g + b,
// as PINNED light when its record carries DrawnLineBuffer::LIGHT_MASK_PINNED_MARK -- a beam
// r_beams_drawn routes, standing in for the per-pixel beam -- else as EMISSIVE light. Alpha 1,
// as the colour's. Nothing above reads the mark: lineLook's int(w + 0.5) drops the quarter.
layout(location = LIGHT_MASK_LOCATION) out vec4 FragLightMask;
#endif

// [F1] hash13 / valueNoise, for the turbulence look.
#include "shaders/glsl/valuenoise.glsl"

void main()
{
	// The view ray through this pixel: the eye to a point on the box face.
	vec3 eye = uCameraPos.xyz;
	vec3 toFrag = vLineWorld - eye;
	float fragDist = length(toFrag);
	if (fragDist < 0.001) discard;
	vec3 dir = toFrag / fragDist;

	vec3 a = vLineA.xyz;
	vec3 b = vLineB.xyz;

	// [F1] The looks this line asked for (header): 1 gradient, 2 turbulence. 0 for
	// every line that asked for none, and then nothing below reads f or g.
	int lineLook = int(vLineTurbulence.w + 0.5);

	// BeamAirGlow's reject, with the ray unbounded (there is no surface distance
	// here). Kept so a line with a negative taper -- fatter than its own reach --
	// is trimmed exactly where the per-pixel beam trims it.
	float thick = max(vLineA.w, 0.01);
	float soft  = max(vLineB.w, 0.01);
	vec3 mid = (a + b) * 0.5;
	float cull = length(b - a) * 0.5 + thick + soft * 6.0 + 1.0;

	// [F1] How much further a look reaches: the halo's swell toward the far end,
	// and licks out to (1 + strength) x the reach. drawnlines.vp grows the box by
	// the same two factors. Taper stays out of it, as in the reject above.
	float swellScale = 1.0;
	float lickScale = 1.0;
	if (lineLook != 0)
	{
		if ((lineLook & 1) != 0) swellScale = max(vLineGradient.w, 1.0);
		if ((lineLook & 2) != 0) lickScale = 1.0 + max(vLineTurbulence.x, 0.0);
		cull = length(b - a) * 0.5 + (thick + soft * swellScale * 6.0 + 1.0) * lickScale;
	}

	vec3 em = mid - eye;
	float along = max(dot(em, dir), 0.0);
	vec3 perp = em - dir * along;
	if (dot(perp, perp) > cull * cull) discard;

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
		// Looking straight down the line: the near end, as BeamAirGlow.
		sc = -dd;
		tc = 0.0;
	}
	else
	{
		sc = (bb * ee - cc * dd) / den;
		tc = (bb * -dd + ee) / den;
	}

	// In front of the eye, and on the segment. NOT clamped to a surface
	// distance -- see note 2 above; the depth written below does that job.
	sc = max(sc, 0.0);
	tc = clamp(tc, 0.0, 1.0);

	float dist = length((eye + dir * sc) - (a + v * tc));

	// The reach before taper, for the automatic depth bias.
	float reach = thick + soft * 6.0 + 1.0;

	// TAPER, SCROLL, FLARE -- as BeamAirGlow.
	float bw = mix(1.0 - vLineLook.z, 1.0, tc);
	thick *= bw;
	soft  *= bw;

	// [F1] GRADIENT: the halo reaches further toward the far end.
	if ((lineLook & 1) != 0)
		soft *= mix(1.0, vLineGradient.w, tc);

	float bright = 1.0;

	if (vLineFX.y > 0.0)
	{
		float alongLine = tc * length(v);
		float s = sin(alongLine * 0.06 - vLineFX.z * vLineFX.x);
		bright *= 1.0 + vLineFX.y * s;
	}

	if (vLineLook.w > 0.0)
		bright += vLineLook.w * pow(clamp(tc, 0.0, 1.0), 8.0);

	// [F1] TURBULENCE: the edges lick (header).
	if ((lineLook & 2) != 0)
	{
		float reachHere = thick + soft * 6.0 + 1.0;
		// Past the widest a lick can pull in, this pixel cannot glow: no noise.
		if (reachHere > 0.0 && dist >= reachHere * lickScale) discard;
		vec3 noisePos = (eye + dir * sc) * vLineTurbulence.y;
		noisePos.y -= uLevelTime.x * vLineTurbulence.z;
		float lickNoise = valueNoise(noisePos) * 0.65 + valueNoise(noisePos * 2.7) * 0.35;
		float lick = (lickNoise * 2.0 - 1.0) * vLineTurbulence.x;
		dist = max(dist - lick * min(dist, reachHere), 0.0);
		bright *= max(1.0 + 0.5 * lick, 0.0);
	}

	float core = 1.0 - smoothstep(thick * 0.5, thick + soft, dist);
	float halo = 1.0 - smoothstep(thick, thick + soft * 6.0 + 1.0, dist);

	// [F1] GRADIENT: the colour runs from the start colour to the end colour.
	vec3 lineColour = vLineCol.rgb;
	if ((lineLook & 1) != 0)
		lineColour = mix(vLineCol.rgb, vLineGradient.rgb, tc);

	vec3 glow = lineColour * (core * 1.6 + halo * vLineLook.y)
		* vLineCol.w * vLineLook.x * bright;

	if (glow == vec3(0.0)) discard;

	// DEPTH: where the glow is, not where the box face is -- the closest
	// approach, pulled toward the eye by the bias, and never nearer than one
	// map unit (so w stays positive). Same mapping gl_Position.z gets on Vulkan:
	// (z + w) / 2 / w.
	float bias = (vLineFX.w < 0.0) ? reach * max(1.0, abs(1.0 - vLineLook.z)) : vLineFX.w;
	// [F1] With a look, the automatic bias is the glow's own reach HERE along the
	// line -- tapered, swollen, licking -- not the whole line's widest: a flame's
	// wide splash stays in front of the wall it hits, and its thin start by the
	// hand is not pulled far toward the eye.
	if (lineLook != 0 && vLineFX.w < 0.0)
		bias = abs(thick + soft * 6.0 + 1.0) * lickScale;
	vec4 clip = ProjectionMatrix * (ViewMatrix * vec4(eye + dir * max(sc - bias, 1.0), 1.0));
	gl_FragDepth = (clip.w > 1e-5) ? clamp((clip.z / clip.w) * 0.5 + 0.5, 0.0, 1.0) : 0.0;

	FragColor = vec4(glow, 1.0);

#ifdef SCENE_LIGHT_MASK
	// [LIGHTMASK] The mark is the quarter above the look flags (hw_drawnlinebuffer.h).
	float glowAmount = glow.r + glow.g + glow.b;
	FragLightMask = (fract(vLineTurbulence.w) > 0.125) ? vec4(0.0, glowAmount, 0.0, 1.0) : vec4(glowAmount, 0.0, 0.0, 1.0);
#endif
#ifdef GBUFFER_PASS
	// Zero with zero alpha: under additive blending this adds nothing to the
	// fog and normal attachments.
	FragFog = vec4(0.0);
	FragNormal = vec4(0.0);
#endif
}
