
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SmokeDepthTexture;	// smokedepth.fp: the linear view depth to march to, this pass's resolution
layout(binding=1) uniform sampler3D DensityLatest;		// the volume: density r, heat g, latest simulation state (linear)
layout(binding=2) uniform sampler3D DensityPrevious;	// the same one simulation step earlier (linear)
layout(binding=3) uniform sampler3D TileActive;			// one texel per tile: 1 = the tile may hold smoke
layout(binding=4) uniform sampler3D SmokeLight;			// [13d] the light grid: rgb the light reaching each place, a its weight (linear)
layout(binding=5) uniform sampler3D SmokeLightDirection;	// [13d] xyz the direction light travels there, times its share (linear)
// [13e] The two drawings that walk the march's rays after it (PPSmokeVolume 3a and 3b), not the march itself.
#if defined(SMOKE_TRANSMITTANCE_CURVE) || defined(SMOKE_BEAM_SCATTER)
layout(binding=6) uniform sampler2D SmokeMarchTexture;	// the blurred march, this resolution: rgb light, a the whole ray's transmittance
#endif
#if defined(SMOKE_TRANSMITTANCE_CURVE)
layout(binding=7) uniform sampler2D SmokeBeamList;		// SMOKE_BEAMS_MAX x 4 RGBA32F texels (hw_framecompute.h, SmokeBeamRecord)
#elif defined(SMOKE_BEAM_SCATTER)
layout(binding=7) uniform sampler2D TransmittanceCurve;	// 3a's output: where the ray's optical depth reaches 0, 1/3, 2/3 and all of it
layout(binding=8) uniform sampler2D SmokeBeamList;		// SMOKE_BEAMS_MAX x 4 RGBA32F texels (hw_framecompute.h, SmokeBeamRecord)
#endif

// ============================================================================
// [SMOKEVOLUME] THE SMOKE VOLUME'S DRAWING, PASS 2 OF 4: THE MARCH.
// ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c; the C++ is PPSmokeVolume.)
//
// For each half-resolution pixel: the light the smoke along the view ray sends toward the eye (rgb),
// and how much of what lies behind still shows through (a, the transmittance T). The output is exactly
// (0, 0, 0, 1) wherever the ray meets no density, and the composite leaves those pixels alone.
//
// THE RAY is built the way heatoffset.fp builds it: TanHalfFov and ProjOffset in view space with z at
// -1, taken out through ViewToWorld. One unit along it is one unit of VIEW DEPTH, so the scene depth
// compares directly. Positions are relative to this eye, in GL world axes. The volume's texel axes are
// Doom's (x, y, z), which is GL (x, z, y).
//
// WHERE IT SAMPLES. The ray is clipped to the grid box and to the depth it stops at. A voxel walk over
// the tile map (TileActive) then finds the first and last tiles along it that may hold smoke, and the
// StepCount samples are spread over that stretch only, so a small cloud in a big empty box gets every
// sample. No two samples sit closer than MinStep (half a cell). The first sample is offset by a
// per-pixel hash: the same screen-space pattern in both eyes, still over time. smokeblur.fp smooths
// what is left of the pattern.
//
// SMOOTH (owner answer 3). Each sample blends the last two simulation states by TicFrac: the previous
// state at 0, the latest at 1, the way an actor's position is interpolated. A blast's shove therefore
// moves at frame rate, not 35 times a second.
//
// LIGHT. Beer-Lambert per sample: the sample's share of opacity (1 minus its own transmittance) times
// the light it scatters toward the eye, dimmed by the smoke already in front of it.
// [13d] The light scattered is LightColor (the look's tint) x the LIGHT GRID there (vk_smokevolume.h,
// shaders/compute/smoke_light.comp: sector light x the look's ambient plus the dynamic lights x its
// scatter, world-aligned, the same for both eyes) x a PHASE: the grid also says which way light travels
// through each place and how much of it does (the direction's length), and that share scatters by
// Henyey-Greenstein -- strongest toward an eye looking back along it -- so a muzzle flash or flashlight
// seen through haze glows toward you. The rest scatters evenly. The phase is scaled so its mean over all
// directions is 1: with light from every side (or none directional), the smoke is exactly as bright as
// tint x light.
//
// [13e] TWO MORE DRAWINGS OF THE SAME RAYS, each its own program (a define), used only while beams or a
// volumetric beam cone meet the smoke ("Engine docs/SMOKE_13E_IMPL_NOTES.md"). Without either define this
// file is the march above, token for token.
//   SMOKE_TRANSMITTANCE_CURVE  the march's own stretch walked again without light: the distances at which the
//       ray's optical depth reaches 0 (the smoke starts), 1/3, 2/3 and all of it (the smoke ends). With the
//       pixel's whole transmittance T, T(t) = T ^ (share of the depth before t / share before the scene):
//       how much haze lies in front of any depth -- a beam, each step of a flashlight cone.
//   SMOKE_BEAM_SCATTER  the light each beam line scatters in the smoke: per beam, the closest approach of this
//       ray and the beam, solved once; three density samples across the beam's glow; the beam's colour and
//       intensity x the density x the look's tint and scatter x the phase x the falloff across the beam x the
//       path through its glow, dimmed by the haze in front of it and by soot (the light grid's w).
// ============================================================================

const float SMOKE_PHASE_G = 0.45;	// the phase's anisotropy: haze scatters forward
const float SMOKE_LIGHT_MAX = 4.0;	// the most light a sample takes from the grid, per channel

float InterleavedGradientNoise(vec2 pixel)
{
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// [13d] Henyey-Greenstein, times 4 pi: 1 on average over the sphere. cosAngle is between the way the light
// travels and the way it leaves toward the eye.
float PhaseHG(float cosAngle)
{
	float g2 = SMOKE_PHASE_G * SMOKE_PHASE_G;
	float denominator = max(1.0 + g2 - 2.0 * SMOKE_PHASE_G * cosAngle, 1e-4);
	return (1.0 - g2) / (denominator * sqrt(denominator));
}

// [13d] The light a sample scatters toward the eye per unit of LightColor: the grid's light there, weighted by
// the phase. The grid covers the same box as the density, so the same normalised coordinate addresses it;
// half a light texel inside on every axis, as DensityAt. towardEye: unit, the volume's (Doom) axes.
vec3 LightAt(vec3 cell, vec3 towardEye, vec3 lightHalfTexel)
{
	vec3 uvw = clamp(cell / GridSize, lightHalfTexel, vec3(1.0) - lightHalfTexel);
	vec3 gridLight = clamp(texture(SmokeLight, uvw).rgb, vec3(0.0), vec3(SMOKE_LIGHT_MAX));
	vec3 weighted = texture(SmokeLightDirection, uvw).xyz;
	float share = length(weighted);
	if (share <= 1e-3)
		return gridLight;
	float phase = PhaseHG(dot(weighted, towardEye) / share);
	share = min(share, 1.0);
	return gridLight * ((1.0 - share) + share * phase);
}

// A ray from the eye (the origin) along the unit direction rd, against the box [boxLo, boxHi]: the
// distances where it enters and leaves. A zero component of rd is taken as a tiny one of either sign,
// which gives the same answer as the exact infinite slab.
bool BoxInterval(vec3 rd, vec3 boxLo, vec3 boxHi, out float tIn, out float tOut)
{
	vec3 dirSign = vec3(greaterThanEqual(rd, vec3(0.0))) * 2.0 - 1.0;
	vec3 invDir = dirSign / max(abs(rd), vec3(1e-8));
	vec3 tA = boxLo * invDir;
	vec3 tB = boxHi * invDir;
	vec3 tLo = min(tA, tB);
	vec3 tHi = max(tA, tB);
	tIn = max(max(tLo.x, tLo.y), tLo.z);
	tOut = min(min(tHi.x, tHi.y), tHi.z);
	return tOut > max(tIn, 0.0);
}

// The grid cell coordinate (texel axes, in cells: cell i spans [i, i + 1)) of an eye-relative GL
// position.
vec3 CellAt(vec3 p)
{
	return ((p - BoxMin) / CellSize).xzy;
}

float DensityAt(vec3 cell)
{
	// Half a texel inside the volume on every axis. The post-process samplers leave W on repeat, which
	// would blend the top layer into the bottom one; this clamp is exactly clamp-to-edge instead.
	vec3 halfTexel = 0.5 / GridSize;
	vec3 uvw = clamp(cell / GridSize, halfTexel, vec3(1.0) - halfTexel);
	float previous = texture(DensityPrevious, uvw).r;
	float latest = texture(DensityLatest, uvw).r;
	return max(mix(previous, latest, TicFrac), 0.0);
}

// The stretch of the ray inside [tIn, tOut] from the first to the last tile it crosses that may hold
// smoke: a voxel walk over the tile map. False when it crosses none.
bool ActiveInterval(vec3 rd, float tIn, float tOut, out float tFirst, out float tLast)
{
	tFirst = 0.0;
	tLast = -1.0;

	ivec3 tiles = ivec3(TileCount + vec3(0.5));
	vec3 tileCells = GridSize / TileCount;
	vec3 entry = (CellAt(rd * tIn)) / tileCells;		// where the ray enters, in tiles
	vec3 dir = ((rd / CellSize).xzy) / tileCells;		// tiles per map unit along the ray
	ivec3 tile = clamp(ivec3(floor(entry)), ivec3(0), tiles - ivec3(1));

	ivec3 stepDir = ivec3(greaterThanEqual(dir, vec3(0.0))) * 2 - ivec3(1);
	vec3 invDir = 1.0 / max(abs(dir), vec3(1e-8));
	// The distance along the ray to each axis' next tile boundary, and between boundaries.
	vec3 forward = vec3(greaterThan(stepDir, ivec3(0)));
	vec3 toBoundary = mix(entry - vec3(tile), vec3(tile) + vec3(1.0) - entry, forward);
	vec3 tNext = vec3(tIn) + max(toBoundary, vec3(0.0)) * invDir;
	vec3 tDelta = invDir;

	bool found = false;
	float tCur = tIn;
	int walk = tiles.x + tiles.y + tiles.z + 1;
	for (int i = 0; i < walk; i++)
	{
		float tLeave = min(min(tNext.x, tNext.y), tNext.z);
		if (texelFetch(TileActive, tile, 0).r > 0.5)
		{
			if (!found)
			{
				tFirst = tCur;
				found = true;
			}
			tLast = min(tLeave, tOut);
		}
		if (tLeave >= tOut)
			break;

		if (tNext.x <= tNext.y && tNext.x <= tNext.z)
		{
			tile.x += stepDir.x;
			tNext.x += tDelta.x;
		}
		else if (tNext.y <= tNext.z)
		{
			tile.y += stepDir.y;
			tNext.y += tDelta.y;
		}
		else
		{
			tile.z += stepDir.z;
			tNext.z += tDelta.z;
		}
		tCur = tLeave;
		if (any(lessThan(tile, ivec3(0))) || any(greaterThanEqual(tile, tiles)))
			break;
	}
	return found;
}

#if defined(SMOKE_TRANSMITTANCE_CURVE) || defined(SMOKE_BEAM_SCATTER)

// [13e] This texel's ray as the march builds it: its unit direction, its stretch inside the box and in front of the
// scene [tIn, tOut], and the scene's distance. False when that stretch is empty.
bool MarchRay(out vec3 rd, out float sceneT, out float tIn, out float tOut)
{
	rd = vec3(0.0, 0.0, -1.0);
	sceneT = 0.0;
	tIn = 0.0;
	tOut = 0.0;
	vec2 ndc = TexCoord * 2.0 - 1.0;
	vec3 viewRay = vec3((ndc + ProjOffset) * TanHalfFov, -1.0);
	vec3 worldRay = mat3(ViewToWorld) * viewRay;
	float stepLen = length(worldRay);
	if (stepLen < 1e-6)
		return false;
	rd = worldRay / stepLen;
	sceneT = texelFetch(SmokeDepthTexture, ivec2(gl_FragCoord.xy), 0).r * stepLen;
	vec3 boxHi = BoxMin + vec3(GridSize.x, GridSize.z, GridSize.y) * CellSize;
	if (!BoxInterval(rd, BoxMin, boxHi, tIn, tOut))
		return false;
	tIn = max(tIn, 0.0);
	tOut = min(tOut, sceneT);
	return tOut > tIn;
}

const int SMOKE_BEAMS_MAX = 16;				// hw_framecompute.h
const float SMOKE_BEAM_SOFT_WIDTHS = 2.0;	// the glow's width in smoke: the core plus this many soft radii
const float SMOKE_BEAM_REACH_WIDTHS = 3.0;	// nothing further from the core than this many widths

// [13e] Whether any listed beam can light, or glow along, this ray between the eye and the scene: the exact least
// distance between the stretch [0, sceneT] and the beam's segment (two segments' closest points, clamped on both)
// within the wider of the beam's air glow reach (main.fp's halo) and its scatter reach, widened by a taper past 1, with
// a quarter to spare plus a half-resolution texel's width at that distance (a full-resolution pixel reads the four
// texels around it). Never false where a beam's glow or scatter can be read.
bool NearAnyBeam(vec3 rd, float sceneT)
{
	int beamCount = min(BeamCount, SMOKE_BEAMS_MAX);
	for (int i = 0; i < beamCount; i++)
	{
		vec4 rowA = texelFetch(SmokeBeamList, ivec2(i, 0), 0);
		vec4 rowB = texelFetch(SmokeBeamList, ivec2(i, 1), 0);
		vec4 look = texelFetch(SmokeBeamList, ivec2(i, 3), 0);
		float thick = max(rowA.w, 0.01);
		float soft = max(rowB.w, 0.01);
		float reach = max(thick + soft * 6.0 + 1.0, (thick + soft * SMOKE_BEAM_SOFT_WIDTHS) * SMOKE_BEAM_REACH_WIDTHS) * max(1.0, abs(1.0 - look.z));

		vec3 d1 = rd * sceneT;				// the ray's stretch, from the eye
		vec3 p2 = BoxMin + rowA.xyz;		// the segment
		vec3 d2 = BoxMin + rowB.xyz - p2;
		vec3 r = -p2;
		float aa = dot(d1, d1);
		float ee = dot(d2, d2);
		float ff = dot(d2, r);
		float s = 0.0;
		float t = 0.0;
		if (ee <= 1e-8)
		{
			s = aa > 1e-8 ? clamp(-dot(d1, r) / aa, 0.0, 1.0) : 0.0;
		}
		else
		{
			float cc = dot(d1, r);
			float bb = dot(d1, d2);
			float den = aa * ee - bb * bb;
			s = den > 1e-8 ? clamp((bb * ff - cc * ee) / den, 0.0, 1.0) : 0.0;
			t = (bb * s + ff) / ee;
			if (t < 0.0)
			{
				t = 0.0;
				s = aa > 1e-8 ? clamp(-cc / aa, 0.0, 1.0) : 0.0;
			}
			else if (t > 1.0)
			{
				t = 1.0;
				s = aa > 1e-8 ? clamp((bb - cc) / aa, 0.0, 1.0) : 0.0;
			}
		}
		float dist = length(d1 * s - (p2 + d2 * t));
		if (dist <= reach * 1.25 + 2.0 + 0.004 * sceneT * s)
			return true;
	}
	return false;
}

#endif

#if defined(SMOKE_TRANSMITTANCE_CURVE)

// [13e] THE TRANSMITTANCE CURVE (PPSmokeVolume 3a). Output: (the distance where the smoke starts, where a third of
// the optical depth lies in front, two thirds, where the smoke ends), map units along the ray; all 0 where this
// march meets no smoke. The same stretch as the march (the tile walk and its margin), half its samples at most,
// the same jitter, no light. Each sample stands for its step [t0 + i dt, t0 + (i + 1) dt]; the thirds fall inside
// their step in proportion.
const int CURVE_STEPS_MAX = 64;
const float CURVE_OPAQUE_DEPTH = 5.5451774;	// ln 256: where the march stops (T < 1/256)

void main()
{
	FragColor = vec4(0.0);

	// A texel the march left exactly clear holds no smoke to describe.
	ivec2 here = ivec2(gl_FragCoord.xy);
	if (texelFetch(SmokeMarchTexture, here, 0) == vec4(0.0, 0.0, 0.0, 1.0))
		return;

	vec3 rd;
	float sceneT;
	float tIn;
	float tOut;
	if (!MarchRay(rd, sceneT, tIn, tOut))
		return;
	// No cone reads the curve this eye: only where a beam can.
	if (NearBeamsOnly > 0.5 && !NearAnyBeam(rd, sceneT))
		return;
	float tFirst;
	float tLast;
	if (!ActiveInterval(rd, tIn, tOut, tFirst, tLast))
		return;
	float t0 = max(tFirst - CellSize, tIn);
	float t1 = min(tLast + CellSize, tOut);
	float span = t1 - t0;
	if (span <= 0.0)
		return;

	int limit = clamp(StepCount / 2, 8, CURVE_STEPS_MAX);
	int count = clamp(int(ceil(span / max(MinStep, 0.001))), 1, limit);
	float dt = span / float(count);
	float jitter = InterleavedGradientNoise(gl_FragCoord.xy);

	float stepDepth[CURVE_STEPS_MAX];
	float total = 0.0;
	int firstStep = -1;
	int lastStep = -1;
	for (int i = 0; i < count; i++)
	{
		float depth = DensityAt(CellAt(rd * (t0 + (float(i) + jitter) * dt))) * Extinction * dt;
		stepDepth[i] = depth;
		if (depth > 0.0)
		{
			if (firstStep < 0)
				firstStep = i;
			lastStep = i;
		}
		total += depth;
		if (total > CURVE_OPAQUE_DEPTH)
			break;
	}
	if (firstStep < 0 || !(total > 0.0))
		return;

	float kStart = t0 + float(firstStep) * dt;
	float kEnd = t0 + float(lastStep + 1) * dt;
	float oneThird = total / 3.0;
	float twoThirds = total * (2.0 / 3.0);
	float kOneThird = kEnd;
	float kTwoThirds = kEnd;
	bool foundOne = false;
	bool foundTwo = false;
	float running = 0.0;
	for (int i = firstStep; i <= lastStep; i++)
	{
		float depth = stepDepth[i];
		float next = running + depth;
		if (depth > 0.0)
		{
			if (!foundOne && next >= oneThird)
			{
				kOneThird = t0 + (float(i) + (oneThird - running) / depth) * dt;
				foundOne = true;
			}
			if (!foundTwo && next >= twoThirds)
			{
				kTwoThirds = t0 + (float(i) + (twoThirds - running) / depth) * dt;
				foundTwo = true;
			}
		}
		running = next;
	}
	kOneThird = clamp(kOneThird, kStart, kEnd);
	kTwoThirds = clamp(kTwoThirds, kOneThird, kEnd);
	FragColor = vec4(kStart, kOneThird, kTwoThirds, kEnd);
}

#elif defined(SMOKE_BEAM_SCATTER)

// [13e] THE TRANSMITTANCE CURVE, READ -- the same two functions in smokemarch.fp, smokecomposite.fp and
// volumetricbeam.fp; keep the three identical. knots: the distances where a ray's optical depth reaches 0, 1/3,
// 2/3 and all of it (knots.w 0 = no smoke seen).
//
// The share of the ray's optical depth lying before distance t: 0 before the smoke, 1 after it, linear between
// the knots (even haze between them).
float SmokeDepthShare(vec4 knots, float t)
{
	if (t <= knots.x)
		return 0.0;
	if (t >= knots.w)
		return 1.0;
	if (t < knots.y)
		return (t - knots.x) / max(knots.y - knots.x, 1e-4) / 3.0;
	if (t < knots.z)
		return (1.0 + (t - knots.y) / max(knots.z - knots.y, 1e-4)) / 3.0;
	return (2.0 + (t - knots.z) / max(knots.w - knots.z, 1e-4)) / 3.0;
}

// The transmittance from the eye to distance t, from the pixel's whole transmittance wholeT and the share of the
// optical depth before its scene distance: exactly wholeT at the scene, 1 where all the smoke lies further away.
// A curve that saw no smoke in front of the scene says nothing, and the whole T stands (the dimming 13c did).
float SmokeTransmittanceTo(vec4 knots, float t, float shareAtScene, float wholeT)
{
	if (wholeT >= 1.0)
		return 1.0;
	if (knots.w <= 0.0 || shareAtScene <= 1e-4)
		return wholeT;
	float share = clamp(SmokeDepthShare(knots, t) / shareAtScene, 0.0, 1.0);
	if (share >= 1.0)
		return wholeT;		// all of it in front: the whole T exactly, not exp(log(T))
	return exp(share * log(max(wholeT, 1e-6)));
}

// [13e] THE LIGHT BEAMS SCATTER IN THE SMOKE (PPSmokeVolume 3b). Output rgb that light, a 1; exactly (0, 0, 0, 1)
// where no beam scatters (and when BeamScatter is 0: r_smoke_beams off). Beam light that meets smoke is sent toward
// the eye in proportion to the smoke's optical depth along this ray's path through the beam's glow:
//   colour x intensity x density x extinction x path x exp(-(distance / width)^2) x phase x (1 - soot darkness)
//   x the transmittance to the beam x the look's tint and scatter x SMOKE_BEAM_SCATTER_GAIN
// width = the core + SMOKE_BEAM_SOFT_WIDTHS soft radii, tapered as the beam's glow; path = width sqrt(pi) / sin(the
// angle between the ray and the beam) -- a Gaussian's integral along the ray, longer the more the ray runs along the
// beam -- never longer than the beam. The phase is 13d's: brightest looking back along the beam's light.
const float SMOKE_BEAM_SCATTER_GAIN = 16.0;	// how much a beam lights haze: density 1 at absorption 1 about as bright as the beam's own core
const float SQRT_PI = 1.7724539;

void main()
{
	FragColor = vec4(0.0, 0.0, 0.0, 1.0);
	if (!(BeamScatter > 0.0) || BeamCount <= 0)
		return;

	ivec2 here = ivec2(gl_FragCoord.xy);
	float wholeT = texelFetch(SmokeMarchTexture, here, 0).a;
	if (!(wholeT < 1.0))
		return;		// no haze on this ray
	vec3 rd;
	float sceneT;
	float tIn;
	float tOut;
	if (!MarchRay(rd, sceneT, tIn, tOut))
		return;

	vec4 knots = texelFetch(TransmittanceCurve, here, 0);
	float shareAtScene = SmokeDepthShare(knots, sceneT);
	vec3 towardEye = -rd;
	vec3 lightHalfTexel = 0.5 / vec3(max(textureSize(SmokeLightDirection, 0), ivec3(1)));

	vec3 scattered = vec3(0.0);
	int beamCount = min(BeamCount, SMOKE_BEAMS_MAX);
	for (int i = 0; i < beamCount; i++)
	{
		vec4 rowA = texelFetch(SmokeBeamList, ivec2(i, 0), 0);
		vec4 rowB = texelFetch(SmokeBeamList, ivec2(i, 1), 0);
		vec4 colour = texelFetch(SmokeBeamList, ivec2(i, 2), 0);
		vec4 look = texelFetch(SmokeBeamList, ivec2(i, 3), 0);
		if (!(colour.w > 0.0) || !(look.x > 0.0))
			continue;

		vec3 a = BoxMin + rowA.xyz;
		vec3 v = BoxMin + rowB.xyz - a;
		float segment = length(v);
		float core = max(rowA.w, 0.01) + max(rowB.w, 0.01) * SMOKE_BEAM_SOFT_WIDTHS;

		// The cheap reject: the segment's middle against this ray's stretch.
		vec3 mid = a + v * 0.5;
		float alongMid = clamp(dot(mid, rd), tIn, tOut);
		vec3 perp = mid - rd * alongMid;
		float cull = segment * 0.5 + core * max(1.0, abs(1.0 - look.z)) * SMOKE_BEAM_REACH_WIDTHS;
		if (dot(perp, perp) > cull * cull)
			continue;

		// The closest approach as BeamAirGlow solves it (the eye at the origin), kept on this ray's stretch, and the
		// point of the segment nearest to it.
		float bb = dot(rd, v);
		float cc = dot(v, v);
		float dd = dot(rd, -a);
		float ee = dot(v, -a);
		float den = cc - bb * bb;
		float sc = abs(den) < 0.0001 ? -dd : (bb * ee - cc * dd) / den;
		sc = clamp(sc, tIn, tOut);
		vec3 onRay = rd * sc;
		float tc = clamp(dot(onRay - a, v) / max(cc, 1e-4), 0.0, 1.0);
		float dist = length(onRay - (a + v * tc));

		float width = max(abs(core * mix(1.0 - look.z, 1.0, tc)), 0.01);
		if (dist >= width * SMOKE_BEAM_REACH_WIDTHS)
			continue;

		vec3 travel = segment > 1e-4 ? v / segment : vec3(0.0, 0.0, -1.0);
		float path = min(width * SQRT_PI / max(length(cross(rd, travel)), 0.02), max(segment, width));
		float halfPath = 0.5 * path;

		// Three samples of the smoke across the glow, as the march reads it (both simulation states, blended).
		float density = 0.25 * DensityAt(CellAt(rd * max(sc - halfPath, tIn)))
			+ 0.5 * DensityAt(CellAt(onRay))
			+ 0.25 * DensityAt(CellAt(rd * min(sc + halfPath, tOut)));
		if (!(density > 0.0))
			continue;

		// Soot sends less back (smoke_light.comp keeps its darkness in the direction grid's w).
		vec3 uvw = clamp(CellAt(onRay) / GridSize, lightHalfTexel, vec3(1.0) - lightHalfTexel);
		float albedo = 1.0 - clamp(texture(SmokeLightDirection, uvw).w, 0.0, 1.0);

		float falloff = exp(-(dist * dist) / (width * width));
		float front = SmokeTransmittanceTo(knots, sc, shareAtScene, wholeT);
		scattered += colour.rgb * (colour.w * density * Extinction * path * falloff * PhaseHG(dot(travel, towardEye)) * albedo * front);
	}
	FragColor = vec4(scattered * LightColor * (BeamScatter * SMOKE_BEAM_SCATTER_GAIN), 1.0);
}

#else

void main()
{
	FragColor = vec4(0.0, 0.0, 0.0, 1.0);

	// This pixel's ray: view space with z at -1, then the same step in world axes.
	vec2 ndc = TexCoord * 2.0 - 1.0;
	vec3 viewRay = vec3((ndc + ProjOffset) * TanHalfFov, -1.0);
	vec3 worldRay = mat3(ViewToWorld) * viewRay;
	float stepLen = length(worldRay);
	if (stepLen < 1e-6)
		return;
	vec3 rd = worldRay / stepLen;

	// Where the scene stops this ray: the depth smokedepth.fp chose for this very texel.
	float sceneT = texelFetch(SmokeDepthTexture, ivec2(gl_FragCoord.xy), 0).r * stepLen;

	vec3 boxHi = BoxMin + vec3(GridSize.x, GridSize.z, GridSize.y) * CellSize;
	float tIn;
	float tOut;
	if (!BoxInterval(rd, BoxMin, boxHi, tIn, tOut))
		return;
	tIn = max(tIn, 0.0);
	tOut = min(tOut, sceneT);

	// r_smoke_debugslice: where the ray meets the level plane SliceHeight (eye-relative) in front of the
	// scene, the density there in false colour, and green where a tile is awake but nearly empty.
	if (DebugSlice != 0 && abs(rd.y) > 1e-6)
	{
		float tSlice = SliceHeight / rd.y;
		if (tSlice > tIn && tSlice < tOut)
		{
			vec3 cell = CellAt(rd * tSlice);
			float density = DensityAt(cell);
			ivec3 tile = clamp(ivec3(floor(cell / (GridSize / TileCount))), ivec3(0), ivec3(TileCount + vec3(0.5)) - ivec3(1));
			float awake = texelFetch(TileActive, tile, 0).r;
			vec3 shade = mix(vec3(0.05, 0.1, 0.4), vec3(1.0, 0.95, 0.8), clamp(density, 0.0, 1.0));
			shade = mix(shade, vec3(0.1, 0.8, 0.2), (1.0 - clamp(density * 8.0, 0.0, 1.0)) * awake * 0.5);
			FragColor = vec4(shade * 0.8, 0.2);
			return;
		}
	}

	if (tOut <= tIn)
		return;

	float tFirst;
	float tLast;
	if (!ActiveInterval(rd, tIn, tOut, tFirst, tLast))
		return;
	// A cell of margin either side, so a tile the walk only grazes (rounding at a corner) is still
	// sampled. Smoke is exactly 0 outside the awake tiles, so the margin costs no accuracy.
	float t0 = max(tFirst - CellSize, tIn);
	float t1 = min(tLast + CellSize, tOut);
	float span = t1 - t0;
	if (span <= 0.0)
		return;

	int count = clamp(int(ceil(span / max(MinStep, 0.001))), 1, max(StepCount, 1));
	float dt = span / float(count);
	float jitter = InterleavedGradientNoise(gl_FragCoord.xy);

	// [13d] The way scattered light leaves toward this eye, in the volume's axes (GL x, z, y), and the light grid's
	// half texel.
	vec3 towardEye = -rd.xzy;
	vec3 lightHalfTexel = 0.5 / vec3(max(textureSize(SmokeLight, 0), ivec3(1)));

	float transmittance = 1.0;
	vec3 light = vec3(0.0);
	for (int i = 0; i < count; i++)
	{
		float t = t0 + (float(i) + jitter) * dt;
		vec3 cell = CellAt(rd * t);
		float density = DensityAt(cell);
		if (density <= 0.0)
			continue;
		float stepTransmittance = exp(-density * Extinction * dt);
		light += transmittance * (1.0 - stepTransmittance) * LightColor * LightAt(cell, towardEye, lightHalfTexel);
		transmittance *= stepTransmittance;
		if (transmittance < 1.0 / 256.0)
			break;
	}
	FragColor = vec4(light, transmittance);
}

#endif
