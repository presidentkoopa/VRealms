/*
** tonemapfilmic.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;

// ============================================================================
// [TONEMAP] A ROLL-OFF INSTEAD OF A CLIP, AND THE COLOUR KEPT.
// ("Engine docs/TONEMAP_IMPL_NOTES.md"; the settings are TonemapFilmicUniforms
// and PPTonemap::FilmicUniforms in the post-process code.)
//
// The scene reaches here in a half-float image, so a lava flat, a glow preset
// built past 1.0, a muzzle flash or the BFG can be well above 1.0 by the time
// post-processing is done.  Nothing between here and the display does anything
// with that headroom: present.fp clamps to 2.0 and the swapchain is 8-bit, so
// every channel over 1.0 lands on 1.0.  That clip is PER CHANNEL, which is why
// a bright orange goes yellow and then white -- red pins first, then green,
// and the pixel walks towards white as it gets brighter.
//
// THE CURVE.  Everything is decided on the pixel's PEAK CHANNEL and applied to
// all three as ONE SCALE, so the ratio between the channels -- the hue and the
// saturation -- is exactly what it was.  The peak is put through
//
//     f(x) = x                              for x <= K
//     f(x) = W - (W-K)^2 / (x - 2K + W)     for x >  K
//
// with K the knee and W what an infinitely bright pixel lands on.  f(K) = K and
// f'(K) = 1, so the curve leaves the straight line without a corner; f is
// increasing everywhere, so the order of two brightnesses is never swapped; and
// f never reaches W, so with W at 1.0 NOTHING CAN CLIP however bright the input.
// Below the knee the pixel is passed through untouched, bit for bit
// (PassThrough) -- wall tones, skin tones and the whole mid-range do not move.
//
// WHY THE PEAK AND NOT A PER-CHANNEL CURVE.  Running a filmic curve on each
// channel on its own -- which is what the Uncharted2 and Hejl-Dawson modes in
// tonemap.fp do -- compresses the bright channel more than the dim ones, so it
// desaturates and skews the hue in the same direction the clip does, only more
// politely.  Scaling by the peak's ratio cannot: every channel is multiplied by
// the same number.  And because the peak itself lands below W, no channel can
// leave the display's gamut, so there is no clip left to hide.
//
// WHY THE PEAK AND NOT THE LUMINANCE.  A luminance curve preserves the ratios
// too, but a saturated colour has a low luminance for its peak, so scaling by a
// luminance ratio leaves the peak channel above 1.0 and the clip comes back on
// exactly the colours this exists to protect.  The peak has no such hole.
//
// DESATURATE is the one dial allowed to move a colour: at 0 (the default) hue
// and saturation are held exactly, and above 0 the deepest part of the shoulder
// is allowed that far towards white, for anyone who wants a blown highlight to
// read as blinding.  It still cannot clip -- mixing towards white raises the
// dim channels and leaves the peak where the curve put it.
//
// A GENERAL PASS.  Nothing here knows what drew the pixel.  It is a curve from
// an image to an image; a beam, a flat, a sprite and a model all meet the same
// arithmetic, and a later pass that wants the same roll-off can call
// TonemapRolloff with its own knee.
// ============================================================================

const float TONEMAP_GAMMA = 2.2;
const float TONEMAP_HALF_MAX = 65504.0;	// the largest finite half float the scene image can hold

// The scene image is not linear -- the same note tonemap.fp carries -- so the
// curve linearizes, works, and re-encodes.  These two are the only thing in
// this file that touches gamma.
vec3 TonemapToLinear(vec3 c)
{
	return pow(c, vec3(TONEMAP_GAMMA));
}

vec3 TonemapFromLinear(vec3 c)
{
	return pow(c, vec3(1.0 / TONEMAP_GAMMA));
}

// The roll-off itself: straight below the knee, a shoulder above it that
// approaches white and never reaches it.
float TonemapRolloff(float x, float knee, float white)
{
	if (x <= knee)
		return x;

	float span = white - knee;
	return white - span * span / (x - knee - knee + white);
}

void main()
{
	vec3 raw = texture(InputTexture, TexCoord).rgb;
	vec3 c = clamp(raw, vec3(0.0), vec3(TONEMAP_HALF_MAX));

	// PASS-THROUGH.  PassThrough is the knee while the exposure is exactly 1,
	// and negative otherwise, so a pixel whose brightest channel is at or below
	// the knee leaves this pass as the bits it arrived with -- not even a gamma
	// round trip to lose a level in.  That is most of a room.
	float peak = max(c.r, max(c.g, c.b));
	if (peak <= PassThrough)
	{
		FragColor = vec4(c, 1.0);
		return;
	}

	vec3 sceneLinear = TonemapToLinear(c) * Exposure;
	float linearPeak = max(sceneLinear.r, max(sceneLinear.g, sceneLinear.b));
	if (linearPeak <= 1e-6)
	{
		FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}

	// KneeLinear and WhiteLinear arrive already linearized and already ordered
	// by the caller; the max is a guard against a value that should never reach
	// here, and costs nothing because it does not vary across the image.
	float knee = KneeLinear;
	float white = max(WhiteLinear, knee + 1e-4);

	float mapped = TonemapRolloff(linearPeak, knee, white);

	// ONE SCALE FOR ALL THREE CHANNELS: the chromaticity is carried through
	// untouched, and its largest component is 1, so the result's largest
	// component is exactly `mapped` and cannot exceed white.
	vec3 ratio = sceneLinear / linearPeak;

	// How deep into the shoulder this pixel sits: 0 at the knee, towards 1 as
	// the input runs away.  The only thing DESATURATE is allowed to follow.
	float shoulder = clamp((mapped - knee) / (white - knee), 0.0, 1.0);
	ratio = mix(ratio, vec3(1.0), clamp(Desaturate, 0.0, 1.0) * shoulder);

	FragColor = vec4(TonemapFromLinear(mapped * ratio), 1.0);
}
