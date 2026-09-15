
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// ============================================================================
// [SMOKE_TEMPORAL] THE SMOKE MARCH'S TEMPORAL ACCUMULATION, PASSES 2b-2d.
// ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E3, "Engine docs/SMOKE_TEMPORAL_E3_IMPL_NOTES.md"; the C++ is
// PPSmokeVolume::Render and PrepareTemporal, the cvar r_smoke_temporal.)
//
// Only while r_smoke_temporal is on. The march (smokemarch.fp SMOKE_TEMPORAL) takes r_smoke_steps samples a ray as
// always, but its first sample's offset turns every frame (each eye's golden-ratio sequence on the same screen pattern),
// so frame after frame it samples between its own earlier samples. This pass blends each frame into that eye's history,
// so the pattern a few steps leave averages away over a few frames.
//
// 2b, THE RESOLVE (no define). Output: the accumulated march, rgb light, a transmittance -- exactly what the march wrote
// without accumulation, so the blur, the transmittance curve, the composite, its light mask carry, the cones and the
// emissive volumes read it unchanged (the 13c contract: scene x T + inscatter, T per pixel).
//   - WHERE THE HISTORY IS. The smoke is a world-space volume, so the camera alone finds last frame's view of this texel's
//     smoke: the point on this texel's ray at the march's REPRESENTATIVE DEPTH (the opacity-weighted mean distance of the
//     smoke along it, carried in the march's alpha; the scene's depth where it met none) is carried by CurrentToPrevious
//     into last frame's view space and projected with that frame's own terms. No motion vectors.
//   - READING IT. Catmull-Rom over the 4x4 texels around that place (sharp: a head turn does not soften the smoke as a
//     bilinear read would, frame after frame), each tap also weighted by how close the depth it was drawn against
//     (PreviousDepthTexture) is to this texel's scene point seen from last frame (the blur's depth weight). The inner 2x2's
//     bilinear share of that agreement is the history's CONFIDENCE: where last frame saw a different surface there, the new
//     frame takes over in proportion; outside last frame's view, all of it.
//   - PARALLAX. One depth reprojects a ray's smoke exactly only if the smoke lies at that depth. Where the history's place
//     at the representative depth and at the scene's depth have moved apart since last frame (the head moved, not just
//     turned), smoke nearer or farther on the ray moved by different amounts, and the new frame weighs more: from
//     TEMPORAL_PARALLAX_FROM texels apart up to TEMPORAL_PARALLAX_BLEND at TEMPORAL_PARALLAX_FULL. A turn moves every depth
//     alike and never raises it.
//   - GHOSTING. The history is clipped toward the box this frame's 3x3 neighbourhood allows, each tap weighted by depth as
//     the blur is: while the history sits within a texel of here (a still head), the neighbourhood's mean +-
//     TEMPORAL_CLIP_STILL deviations, wide enough for the march's own sample noise to average away; carried from two texels
//     or more (turning, strafing), also inside the neighbourhood's min and max and TEMPORAL_CLIP_MOVING deviations.
//   - SURPRISE. A history far outside what the neighbourhood's mean could be off by (its deviation / 3, the standard error
//     of nine samples) is stale in a way no box catches -- the light itself changed: a muzzle flash, a lamp -- and the new
//     frame weighs up to TEMPORAL_SURPRISE_BLEND.
//   - WHERE THE SMOKE ITSELF CHANGED. The march's CHANGE LEVEL (0..3: how much of the smoke the ray reaches the
//     simulation's last step changed -- a round carving it, a blast shoving it), the largest among this texel and its
//     neighbours at its depth, raises the new frame's weight up to TEMPORAL_CHANGE_BLEND.
//   - CLEAR STAYS CLEAR. A texel whose 3x3 neighbourhood this frame is all exactly clear (0, 0, 0, 1) is written as
//     exactly that constant, whatever the history held, so the blur and the composite still leave those pixels alone
//     bit for bit (the laser look outside the smoke).
//   - NO HISTORY (HistoryValid 0: the first frame, a new size, a cut, a teleport, a missed frame): this frame's march.
//
// 2c and 2d, SMOKE_TEMPORAL_KEEP: a plain copy, texel for texel, of the resolved march into the eye's history and of this
// frame's smokedepth.fp output into the eye's previous depth, for the next frame.
// ============================================================================

#if defined(SMOKE_TEMPORAL_KEEP)

layout(binding=0) uniform sampler2D SourceTexture;

void main()
{
	FragColor = texelFetch(SourceTexture, ivec2(gl_FragCoord.xy), 0);
}

#else

layout(binding=0) uniform sampler2D TemporalMarchTexture;	// smokemarch.fp SMOKE_TEMPORAL: rgb light, a T + 2 x (depth code + 256 x change level) (RGBA32F)
layout(binding=1) uniform sampler2D SmokeDepthTexture;		// smokedepth.fp this frame: the view depth each texel marched to
layout(binding=2) uniform sampler2D HistoryTexture;			// this eye's resolved march last frame (RGBA16F)
layout(binding=3) uniform sampler2D PreviousDepthTexture;	// this eye's smokedepth.fp output last frame (R32F)

const float SMOKE_TEMPORAL_CODES_PER_DOUBLING = 16.0;	// smokemarch.fp: the depth code's steps per doubling of depth
const float TEMPORAL_BLEND = 0.15;			// the new frame's weight over a history that fully matches (about 6 frames' memory)
const float TEMPORAL_CLIP_STILL = 1.5;		// the box while the history moved under a texel: the mean +- this many deviations
const float TEMPORAL_CLIP_MOVING = 1.25;	// the box from two texels of motion: inside the min and max, and the mean +- this many
const float TEMPORAL_MOVING_FROM = 1.0;		// texels of motion where the box starts to tighten (fully at one more)
const float TEMPORAL_PARALLAX_FROM = 0.5;	// texels between the smoke's and the scene's reprojected places where parallax starts to count
const float TEMPORAL_PARALLAX_FULL = 2.0;	// ... and where it counts fully
const float TEMPORAL_PARALLAX_BLEND = 0.8;	// the new frame's weight at full parallax
const float TEMPORAL_SURPRISE_BLEND = 0.8;	// the new frame's weight at full surprise
const float TEMPORAL_CHANGE_BLEND = 0.6;	// the new frame's weight at the top change level (3)
const vec4 CLEAR_TEXEL = vec4(0.0, 0.0, 0.0, 1.0);

// A texel of this frame's temporal march: (light, transmittance), and the representative depth and change level its alpha
// carries.
vec4 TemporalMarchAt(ivec2 texel, out float depth, out float changeLevel)
{
	vec4 stored = texelFetch(TemporalMarchTexture, texel, 0);	// not "packed": a reserved word NVIDIA's GL compiler rejects
	float code = floor(stored.a * 0.5);
	changeLevel = floor(code / 256.0);
	depth = pow(2.0, (code - 256.0 * changeLevel) / SMOKE_TEMPORAL_CODES_PER_DOUBLING);
	return vec4(stored.rgb, clamp(stored.a - 2.0 * code, 0.0, 1.0));
}

// smokeblur.fp's depth weight, the same text: 1 at the same depth, a half 5% apart.
float DepthWeight(float centre, float other)
{
	float relative = abs(other - centre) / max(min(other, centre), 1.0);
	return 1.0 / (1.0 + 400.0 * relative * relative);
}

// Catmull-Rom's four tap weights for a sample the fraction f of the way from the second tap to the third.
vec4 CatmullRomWeights(float f)
{
	return vec4(f * (-0.5 + f * (1.0 - 0.5 * f)), 1.0 + f * f * (-2.5 + 1.5 * f), f * (0.5 + f * (2.0 - 1.5 * f)), f * f * (-0.5 + 0.5 * f));
}

// The value moved toward the box's centre until it lies inside the box (on every channel at once, so the colour keeps its
// hue); unchanged when it already does.
vec4 ClipTowardBox(vec4 value, vec4 lo, vec4 hi)
{
	vec4 centre = 0.5 * (lo + hi);
	vec4 extent = max(0.5 * (hi - lo), vec4(1e-6));
	vec4 offset = value - centre;
	vec4 units = abs(offset) / extent;
	float reach = max(max(units.r, units.g), max(units.b, units.a));
	return reach > 1.0 ? centre + offset / reach : value;
}

// A point of last frame's view space as a texel position of this pass (texel centres at whole numbers).
vec2 PreviousTexelAt(vec4 previousView, ivec2 size)
{
	vec2 previousNdc = previousView.xy / (-previousView.z) / PreviousTanHalfFov - PreviousProjOffset;
	return (previousNdc * 0.5 + 0.5) * vec2(size) - vec2(0.5);
}

void main()
{
	ivec2 size = textureSize(TemporalMarchTexture, 0);
	ivec2 here = ivec2(gl_FragCoord.xy);
	float centreDepth = texelFetch(SmokeDepthTexture, here, 0).r;

	float marchDepth;
	float change;
	vec4 current = TemporalMarchAt(here, marchDepth, change);

	// This frame's 3x3 neighbourhood: any smoke at all, its depth-weighted mean and deviation, and the min, the max and the
	// largest change level of the taps at this texel's depth.
	bool anySmoke = false;
	vec4 sum = vec4(0.0);
	vec4 squares = vec4(0.0);
	float weightSum = 0.0;
	vec4 lo = current;
	vec4 hi = current;
	for (int j = -1; j <= 1; j++)
	{
		for (int i = -1; i <= 1; i++)
		{
			ivec2 tap = clamp(here + ivec2(i, j), ivec2(0), size - ivec2(1));
			float tapDepth;
			float tapChange;
			vec4 s = TemporalMarchAt(tap, tapDepth, tapChange);
			if (s != CLEAR_TEXEL)
				anySmoke = true;
			float w = DepthWeight(centreDepth, texelFetch(SmokeDepthTexture, tap, 0).r);
			sum += s * w;
			squares += s * s * w;
			weightSum += w;
			if (w >= 0.5)
			{
				lo = min(lo, s);
				hi = max(hi, s);
				change = max(change, tapChange);
			}
		}
	}
	if (!anySmoke)
	{
		FragColor = CLEAR_TEXEL;
		return;
	}
	if (HistoryValid == 0)
	{
		FragColor = current;
		return;
	}

	// This texel's ray as the march builds it (view space, z at -1: one unit along it is one unit of view depth).
	vec2 ndc = TexCoord * 2.0 - 1.0;
	vec3 viewRay = vec3((ndc + ProjOffset) * TanHalfFov, -1.0);
	// Where its smoke sits: the representative depth, never behind the scene; the scene where the ray met none.
	float reprojectDepth = current.a < 1.0 ? min(marchDepth, centreDepth) : centreDepth;
	vec4 previousPoint = CurrentToPrevious * vec4(viewRay * reprojectDepth, 1.0);
	vec4 previousScene = CurrentToPrevious * vec4(viewRay * centreDepth, 1.0);

	float newWeight = 1.0;
	float moved = 0.0;
	float parallax = 0.0;
	vec4 history = current;
	if (previousPoint.z < -1e-3 && previousScene.z < -1e-3)
	{
		vec2 at = PreviousTexelAt(previousPoint, size);
		if (all(greaterThanEqual(at, vec2(-0.5))) && all(lessThanEqual(at, vec2(size) - vec2(0.5))))
		{
			vec2 corner = floor(at);
			vec2 along = at - corner;
			ivec2 cornerTexel = ivec2(corner);
			float expected = -previousScene.z;
			vec4 wx = CatmullRomWeights(along.x);
			vec4 wy = CatmullRomWeights(along.y);
			vec4 cubicSum = vec4(0.0);
			float cubicWeight = 0.0;
			vec4 bilinearSum = vec4(0.0);
			float matched = 0.0;
			for (int j = 0; j < 4; j++)
			{
				for (int i = 0; i < 4; i++)
				{
					ivec2 tap = clamp(cornerTexel + ivec2(i - 1, j - 1), ivec2(0), size - ivec2(1));
					float depthMatch = DepthWeight(expected, texelFetch(PreviousDepthTexture, tap, 0).r);
					vec4 value = texelFetch(HistoryTexture, tap, 0);
					float w = wx[i] * wy[j] * depthMatch;
					cubicSum += value * w;
					cubicWeight += w;
					if (i >= 1 && i <= 2 && j >= 1 && j <= 2)
					{
						float bilinear = (i == 1 ? 1.0 - along.x : along.x) * (j == 1 ? 1.0 - along.y : along.y) * depthMatch;
						bilinearSum += value * bilinear;
						matched += bilinear;
					}
				}
			}
			if (matched > 0.02)
			{
				// The cubic read where enough of its weight agrees on depth; the depth-matched bilinear read where it does not.
				history = cubicWeight > 0.25 ? cubicSum / cubicWeight : bilinearSum / matched;
				newWeight = 1.0 - (1.0 - TEMPORAL_BLEND) * clamp(matched, 0.0, 1.0);
				moved = length(at - vec2(here));
				parallax = length(PreviousTexelAt(previousScene, size) - at);
			}
		}
	}

	// The box: the mean's deviations while still, inside the min and max too from two texels of motion.
	vec4 mean = sum / weightSum;
	vec4 deviation = sqrt(max(squares / weightSum - mean * mean, vec4(0.0)));
	float moving = clamp(moved - TEMPORAL_MOVING_FROM, 0.0, 1.0);
	vec4 boxLo = mix(mean - TEMPORAL_CLIP_STILL * deviation, max(lo, mean - TEMPORAL_CLIP_MOVING * deviation), moving);
	vec4 boxHi = mix(mean + TEMPORAL_CLIP_STILL * deviation, min(hi, mean + TEMPORAL_CLIP_MOVING * deviation), moving);
	boxLo = min(boxLo, current);
	boxHi = max(boxHi, current);

	// Surprise, measured before the clip: how many standard errors of the neighbourhood's mean the history sits away from it
	// (with a floor of 2% of the mean and 0.002, so a quantisation step or a near-clear texel never reads as surprise).
	vec4 away = abs(history - mean) / (deviation / 3.0 + 0.02 * abs(mean) + vec4(0.002));
	float surprise = smoothstep(3.0, 6.0, max(max(away.r, away.g), max(away.b, away.a)));

	history = ClipTowardBox(history, boxLo, boxHi);
	newWeight = max(newWeight, TEMPORAL_SURPRISE_BLEND * surprise);
	// Where the head moved and the ray's smoke and surface parted since last frame, one depth cannot carry all of it.
	newWeight = max(newWeight, TEMPORAL_PARALLAX_BLEND * smoothstep(TEMPORAL_PARALLAX_FROM, TEMPORAL_PARALLAX_FULL, parallax));
	// Where the simulation's last step changed this texel's smoke, the new frame weighs more.
	newWeight = max(newWeight, TEMPORAL_CHANGE_BLEND * change / 3.0);

	vec4 result = mix(history, current, newWeight);
	if (any(isnan(result)) || any(isinf(result)))
		result = current;
	FragColor = vec4(max(result.rgb, vec3(0.0)), clamp(result.a, 0.0, 1.0));
}

#endif
