
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
// [EMISSIVEVOLUMES] SMOKE_CURVE_NEAR_VOLUMES (with SMOKE_TRANSMITTANCE_CURVE): an emissive volume is drawn this eye and reads the
// curve, so it is also marched where a volume can read it ("Engine docs/EMISSIVE_VOLUMES_15_IMPL_NOTES.md"). Without the define
// this file is the curve, the scatter and the march above and below, token for token.
#if defined(SMOKE_CURVE_NEAR_VOLUMES)
layout(binding=8) uniform sampler2D EmissiveVolumeList;	// EMISSIVE_VOLUMES_DRAWN_MAX x EMISSIVE_VOLUME_TEXELS RGBA32F (hw_emissivevolumeframe.h)
#endif
// [SHAREDMARCH] E2 ("Engine docs/SHARED_MARCH_E2_IMPL_NOTES.md"): SMOKE_SHARED_FILL reads what the carry left in this eye's
// texels (shaders/pp/sharedmarchwarp.fp), so it marches only the near shell -- or the whole ray where the carry left a hole.
#if defined(SMOKE_SHARED_FILL)
layout(binding=6) uniform sampler2D SharedMarchCarried;
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

// [SMOKE_TEMPORAL] SMOKE_TEMPORAL (with none of the defines above): the march for temporal accumulation (PPSmokeVolume,
// smoketemporal.fp, "Engine docs/SMOKE_TEMPORAL_E3_IMPL_NOTES.md"). The light and the transmittance are the march's, sample for
// sample; the differences:
//   - the first sample's offset turns by JitterOffset every frame (this eye's golden-ratio sequence on the same screen pattern);
//   - the alpha carries two more things for the resolve: the ray's REPRESENTATIVE DEPTH -- the opacity-weighted mean distance of
//     its smoke, in view depth -- so the resolve finds this texel's smoke in last frame's view, and its CHANGE LEVEL -- how much
//     of the smoke the ray can reach the simulation's last step changed (a round carving it, a blast shoving it), each place
//     weighed by how much of the ray it could hide at the larger of the two states -- so the resolve trusts the history less
//     there. alpha = T + 2 x (code + 256 x level): code 0..255 in sixteenths of a doubling of depth, level 0..3. T comes back
//     within a float32 step of the alpha (at most 1.2e-4); a ray that met no smoke has T exactly 1 and light exactly 0, so the
//     resolve still reads a clear texel as (0, 0, 0, 1). Its target is RGBA32F.
// Without the define this file is the march, token for token.
// [SHAREDMARCH] E2: SMOKE_SHARED and SMOKE_SHARED_FILL want the same two measures the temporal march keeps -- the ray's
// representative depth and how much of its smoke the last simulation step changed -- so this lump is compiled for them too.
// With none of the three defines the tokens below are absent, exactly as before.
#if defined(SMOKE_TEMPORAL) || defined(SMOKE_SHARED) || defined(SMOKE_SHARED_FILL)
const float SMOKE_TEMPORAL_CODES_PER_DOUBLING = 16.0;	// smoketemporal.fp
const float SMOKE_TEMPORAL_CHANGE_FLOOR = 0.05;	// a change in density thinner than this counts against this, not against itself
const float SMOKE_TEMPORAL_CHANGE_FULL = 0.5;	// the share of the reachable smoke that changed which reads as the top level

// The two simulation states at a cell, and the density DensityAt gives there (its same expression).
float DensityStatesAt(vec3 cell, out float previous, out float latest)
{
	vec3 halfTexel = 0.5 / GridSize;
	vec3 uvw = clamp(cell / GridSize, halfTexel, vec3(1.0) - halfTexel);
	previous = texture(DensityPrevious, uvw).r;
	latest = texture(DensityLatest, uvw).r;
	return max(mix(previous, latest, TicFrac), 0.0);
}

float SmokeTemporalPack(float transmittance, float depth, float changeShare)
{
	float code = transmittance < 1.0 ? clamp(floor(log2(max(depth, 1.0)) * SMOKE_TEMPORAL_CODES_PER_DOUBLING + 0.5), 0.0, 255.0) : 0.0;
	float level = clamp(floor(changeShare / SMOKE_TEMPORAL_CHANGE_FULL * 3.0 + 0.5), 0.0, 3.0);
	return clamp(transmittance, 0.0, 1.0) + 2.0 * (code + 256.0 * level);
}
#endif

#if defined(SMOKE_SHARED) || defined(SMOKE_SHARED_FILL)
// [SHAREDMARCH] E2: THE PACKED MARCH, the one format every caller writes and PPSharedMarchWarp carries. Kept token for
// token in smokemarch.fp, emissivevolume.fp and sharedmarchwarp.fp (a post-process lump has no #include); the E2 proofs
// compare the three texts.
//
// Two halves a channel, which is the precision the march's own RGBA16F target always had:
//   r  light.r, light.g      g  light.b, transmittance      b  front depth, back depth      a  depth, change
// Every value is clamped to a finite half before it is packed, so no channel can become a float32 NaN (a NaN would need
// the HIGH half of a channel to be an infinity, and the high halves are the ones clamped to a range). A ray that met
// nothing packs to light 0, transmittance 1 and zero depths, which unpacks to exactly that. Depth 0 means the texel
// carries nothing; SHARED_MARCH_HOLE means the carry could not fill it.
//
// BACK CARRIES A SIGN, and it is what tells a disocclusion from an ordinary end. POSITIVE: the ray ran to its stop
// with light still in it, so the scene is what ended it there and anything beyond that stop is hidden -- an eye whose
// own scene reaches past it is looking around an edge at light this march never got to, and the carry marks a hole.
// NEGATIVE: the light simply ran out before the scene did, and an eye seeing further sees nothing more. Only the
// magnitude is the depth.
const float SHARED_MARCH_HOLE = -1.0;
const float SHARED_MARCH_HALF_MAX = 60000.0;

struct SharedMarchSample
{
	vec3 Light;
	float Transmittance;
	float Front;
	float Back;
	float Depth;
	float Change;
};

vec4 SharedMarchPack(vec3 light, float transmittance, float front, float back, float depth, float change)
{
	vec3 safeLight = clamp(light, vec3(0.0), vec3(SHARED_MARCH_HALF_MAX));
	return vec4(
		uintBitsToFloat(packHalf2x16(safeLight.rg)),
		uintBitsToFloat(packHalf2x16(vec2(safeLight.b, clamp(transmittance, 0.0, 1.0)))),
		uintBitsToFloat(packHalf2x16(vec2(clamp(front, 0.0, SHARED_MARCH_HALF_MAX), clamp(back, -SHARED_MARCH_HALF_MAX, SHARED_MARCH_HALF_MAX)))),
		uintBitsToFloat(packHalf2x16(vec2(clamp(depth, SHARED_MARCH_HOLE, SHARED_MARCH_HALF_MAX), clamp(change, 0.0, 1.0)))));
}

SharedMarchSample SharedMarchUnpack(vec4 texel)
{
	vec2 lightRG = unpackHalf2x16(floatBitsToUint(texel.r));
	vec2 blueAndT = unpackHalf2x16(floatBitsToUint(texel.g));
	vec2 frontBack = unpackHalf2x16(floatBitsToUint(texel.b));
	vec2 depthChange = unpackHalf2x16(floatBitsToUint(texel.a));
	SharedMarchSample result;
	result.Light = vec3(lightRG, blueAndT.x);
	result.Transmittance = blueAndT.y;
	result.Front = frontBack.x;
	result.Back = frontBack.y;
	result.Depth = depthChange.x;
	result.Change = depthChange.y;
	return result;
}
#endif

#if defined(SMOKE_SHARED_FILL)
// [SHAREDMARCH] E2: the fill writes what the eye's march always wrote -- the plain (light, transmittance) of the march, or
// the temporal march's packed alpha when this eye is accumulating (TemporalOut). Every pass after it reads the same texture
// it always did, in the same format.
vec4 SmokeSharedOut(vec3 light, float transmittance, float depth, float change)
{
	return TemporalOut != 0 ? vec4(light, SmokeTemporalPack(transmittance, depth, change)) : vec4(light, transmittance);
}
#endif

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

#if defined(SMOKE_CURVE_NEAR_VOLUMES)
const int EMISSIVE_VOLUMES_DRAWN_MAX = 32;	// hw_emissivevolumeframe.h

// [EMISSIVEVOLUMES] Whether any listed emissive volume's bounding sphere can be read on this ray between the eye and the scene:
// the least distance from the sphere's centre (the list's rows 0 and 1: base + axis x centre along it) to the stretch
// [0, sceneT], within the radius with a quarter to spare plus a half-resolution texel's width at that distance, as NearAnyBeam.
// Never false where a volume's march reads the curve.
bool NearAnyVolume(vec3 rd, float sceneT)
{
	int volumeCount = min(VolumeCount, EMISSIVE_VOLUMES_DRAWN_MAX);
	for (int i = 0; i < volumeCount; i++)
	{
		vec4 rowBase = texelFetch(EmissiveVolumeList, ivec2(i, 0), 0);
		vec4 rowAxis = texelFetch(EmissiveVolumeList, ivec2(i, 1), 0);
		if (!(rowBase.w > 0.0))
			continue;
		vec3 centre = VolumeOrigin + rowBase.xyz + rowAxis.xyz * rowAxis.w;
		float along = clamp(dot(centre, rd), 0.0, sceneT);
		if (length(centre - rd * along) <= rowBase.w * 1.25 + 2.0 + 0.004 * along)
			return true;
	}
	return false;
}
#endif

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
#if defined(SMOKE_CURVE_NEAR_VOLUMES)
	// [EMISSIVEVOLUMES] Or where an emissive volume can.
	if (NearBeamsOnly > 0.5 && !NearAnyBeam(rd, sceneT) && !NearAnyVolume(rd, sceneT))
		return;
#else
	if (NearBeamsOnly > 0.5 && !NearAnyBeam(rd, sceneT))
		return;
#endif
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

// ============================================================================
// [FOVEATED] E4: FIXED FOVEATED EFFECTS ("Engine docs/FOVEATED_E4_IMPL_NOTES.md"; r_effects_foveated).
//
// A headset lens is sharp down the middle and blurred and stretched at its rim, so a ray far off the lens axis does not
// need as many samples as one down the middle.  This scales the march's STEP CAP by the angle between the ray and that
// axis: full inside FOVEATED_FULL_TAN (25 degrees), falling smoothly to the published edge share by FOVEATED_EDGE_TAN
// (50 degrees).  Everything below the two lines is untouched: the same stretch, the same jitter, the same light.
//
// THE CENTRE IS THE LENS AXIS, AND IT IS EXACT.  A view ray is (ndc + ProjOffset) * TanHalfFov, so the ray ALONG the lens
// axis is the one whose two tangents are zero -- which is where a headset's asymmetric eye really puts it, not the middle
// of the buffer.  The angle below is therefore the true angle off the axis, and it needs no uniform, no screen centre and
// no assumption that the frustum is symmetric.  It is also why this is right for E2's shared march, which serves both
// eyes at once: the two eyes look the same way, so their two lens axes are the same DIRECTION and land on the same texel
// of the shared buffer, whichever eye's projection that buffer was drawn with.  A headset that reports gaze later moves
// the centre by subtracting a tangent-space offset from tanOff, and nothing else here changes.
//
// IT SCALES THE CAP, NOT THE STEP SIZE, AND THAT IS WHAT KEEPS NEAR SMOKE EXACT.  The march takes
// ceil(span / MinStep) samples and only the cap holds a long ray back, so a short stretch -- a puff at your feet, a
// grenade by your boot -- is still sampled at half a cell whatever the angle, and only rays long enough to be capped
// take fewer.  Nothing near the head loses a sample, at any angle, and no near test is needed to arrange it.
//
// OFF is the first line.  With nothing published (0), or a share of 1 or more, this returns max(steps, 1) -- the
// expression the march always used, so every count, every dt, every sample position and every fetch is the same bits.
// ============================================================================
const float FOVEATED_FULL_TAN = 0.46631;	// tan(25 degrees): full quality inside this angle off the lens axis
const float FOVEATED_EDGE_TAN = 1.19175;	// tan(50 degrees): the edge share from here outward
const int FOVEATED_MIN_STEPS = 8;			// never fewer than this, whatever the angle (and never more than steps)

// This texel's step cap.  tanOff is the ray's two tangents off the lens axis -- viewRay.xy, which the march already has.
int FoveatedStepCap(int steps, vec2 tanOff, float edgeShare)
{
	int full = max(steps, 1);
	if (!(edgeShare > 0.0) || edgeShare >= 1.0)
		return full;
	float away = smoothstep(FOVEATED_FULL_TAN, FOVEATED_EDGE_TAN, length(tanOff));
	return clamp(int(float(full) * mix(1.0, edgeShare, away) + 0.5), min(FOVEATED_MIN_STEPS, full), full);
}

void main()
{
	FragColor = vec4(0.0, 0.0, 0.0, 1.0);
#if defined(SMOKE_SHARED)
	// [SHAREDMARCH] E2: a texel of the shared march that meets nothing is the packed clear, not the plain one.
	FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, 0.0, 0.0);
#endif
#if defined(SMOKE_SHARED_FILL)
	// [SHAREDMARCH] E2: what the carry left in this texel. A HOLE means the carry could not serve this eye here and this
	// pass marches the whole ray itself at FillStepCount; otherwise the carried stretch stands and only the near shell is
	// marched here and put over it. Either way this is what the texel says if nothing below adds to it, so every early
	// return below keeps the carry.
	SharedMarchSample carried = SharedMarchUnpack(texelFetch(SharedMarchCarried, ivec2(gl_FragCoord.xy), 0));
	bool carriedHole = carried.Depth <= SHARED_MARCH_HOLE * 0.5;
	FragColor = carriedHole ? SmokeSharedOut(vec3(0.0), 1.0, 0.0, 0.0) :
		SmokeSharedOut(carried.Light, carried.Transmittance, carried.Depth, carried.Change);
#endif

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
#if defined(SMOKE_SHARED_FILL)
	// [SHAREDMARCH] E2: with the carry standing, this eye marches only the NEAR SHELL -- the stretch nearer than the split,
	// which is where the two eyes really differ and where the carry's error would be largest. A hole marches the whole ray.
	if (!carriedHole)
		tOut = min(tOut, NearSplit);
	if (tOut <= tIn)
		return;
#endif

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
#if defined(SMOKE_SHARED)
	// [SHAREDMARCH] E2: the shared march is the stretch PAST the split. What lies nearer is marched by each eye itself and
	// put over this, so nothing is counted twice and nothing near is carried.
	t0 = max(t0, NearSplit);
#endif
	float span = t1 - t0;
	if (span <= 0.0)
		return;

	// [FOVEATED] E4: the only line this step changes in the march.  max(StepCount, 1) became the cap this texel's angle off
	// the lens axis asks for; with the switch off FoveatedStepCap returns max(StepCount, 1) on its first line.  MarchPad0 is
	// the march block's reserved word, which SetupSmokeVolume fills with the edge share (hw_drawinfo.cpp).
	int count = clamp(int(ceil(span / max(MinStep, 0.001))), 1, FoveatedStepCap(StepCount, viewRay.xy, MarchPad0));
	float dt = span / float(count);
#if defined(SMOKE_SHARED_FILL)
	// [SHAREDMARCH] E2: A HOLE WALKS TWO STRETCHES, and the loop below walks them one after the other in a single pass: the
	// NEAR SHELL at the march's own step size, then the far stretch at the fill's lower one. Marching the whole ray at the
	// fill's count instead puts eighty-seven units between samples at headset range and steps straight over a smoke grenade
	// at your feet -- the mirror measured exactly that before this was split in two (M4b). Front to back in one loop, so the
	// two stretches compose the way two parts of one ray always did. Where the carry stands, the far count is zero and every
	// number here is the one the march always used.
	float nearStart = t0;
	float nearEnd = carriedHole ? min(t1, NearSplit) : t1;
	float farStart = carriedHole ? max(t0, NearSplit) : 0.0;
	float farEnd = carriedHole ? t1 : 0.0;
	float nearSpan = max(nearEnd - nearStart, 0.0);
	float farSpan = max(farEnd - farStart, 0.0);
	int nearCount = nearSpan > 0.0 ? clamp(int(ceil(nearSpan / max(MinStep, 0.001))), 1, max(StepCount, 1)) : 0;
	int farCount = farSpan > 0.0 ? clamp(int(ceil(farSpan / max(MinStep, 0.001))), 1, max(FillStepCount, 1)) : 0;
	float dtNear = nearCount > 0 ? nearSpan / float(nearCount) : 0.0;
	float dtFar = farCount > 0 ? farSpan / float(farCount) : 0.0;
	count = nearCount + farCount;
	if (count <= 0)
		return;
	dt = nearCount > 0 ? dtNear : dtFar;
#endif
	float jitter = InterleavedGradientNoise(gl_FragCoord.xy);
#if defined(SMOKE_TEMPORAL) || defined(SMOKE_SHARED) || defined(SMOKE_SHARED_FILL)
	// [SHAREDMARCH] E2: the shared march and the fill take the same turned offset. With temporal accumulation off the
	// renderer publishes 0 here and the pattern is the fixed one the march always used.
	jitter = fract(jitter + JitterOffset);	// [SMOKE_TEMPORAL] a new offset every frame
#endif

	// [13d] The way scattered light leaves toward this eye, in the volume's axes (GL x, z, y), and the light grid's
	// half texel.
	vec3 towardEye = -rd.xzy;
	vec3 lightHalfTexel = 0.5 / vec3(max(textureSize(SmokeLight, 0), ivec3(1)));

	float transmittance = 1.0;
	vec3 light = vec3(0.0);
#if defined(SMOKE_TEMPORAL) || defined(SMOKE_SHARED) || defined(SMOKE_SHARED_FILL)
	float depthSum = 0.0;	// [SMOKE_TEMPORAL] each sample's distance x its share of the ray's opacity
	float changeSum = 0.0;	// [SMOKE_TEMPORAL] each sample's reach x the share of its density the last simulation step changed
	float reachSum = 0.0;
#endif
#if defined(SMOKE_SHARED)
	// [SHAREDMARCH] E2: the ends of the stretch the light came from, which is what lets an eye whose own scene cuts the ray
	// shorter take the right share of the optical depth instead of all of it.
	float hitFirst = 0.0;
	float hitLast = 0.0;
	bool hitAny = false;
#endif
	for (int i = 0; i < count; i++)
	{
#if defined(SMOKE_SHARED_FILL)
		// [SHAREDMARCH] E2: the two stretches, one after the other. dt changes with them, so each sample's optical depth is
		// its own step's, and the near shell keeps the march's step size even inside a hole.
		if (i == nearCount)
			dt = dtFar;
		float t = i < nearCount ? nearStart + (float(i) + jitter) * dtNear : farStart + (float(i - nearCount) + jitter) * dtFar;
#else
		float t = t0 + (float(i) + jitter) * dt;
#endif
		vec3 cell = CellAt(rd * t);
#if defined(SMOKE_TEMPORAL) || defined(SMOKE_SHARED) || defined(SMOKE_SHARED_FILL)
		float previousDensity;
		float latestDensity;
		float density = DensityStatesAt(cell, previousDensity, latestDensity);
		// [SMOKE_TEMPORAL] The reach: how much of the ray this step could hide at the larger state, behind what lies in front.
		float largest = max(max(previousDensity, latestDensity), 0.0);
		float reach = transmittance * min(largest * Extinction * dt, 1.0);
		changeSum += reach * abs(latestDensity - previousDensity) / max(largest, SMOKE_TEMPORAL_CHANGE_FLOOR);
		reachSum += reach;
#else
		float density = DensityAt(cell);
#endif
		if (density <= 0.0)
			continue;
		float stepTransmittance = exp(-density * Extinction * dt);
		light += transmittance * (1.0 - stepTransmittance) * LightColor * LightAt(cell, towardEye, lightHalfTexel);
#if defined(SMOKE_TEMPORAL) || defined(SMOKE_SHARED) || defined(SMOKE_SHARED_FILL)
		depthSum += transmittance * (1.0 - stepTransmittance) * t;
#endif
#if defined(SMOKE_SHARED)
		// [SHAREDMARCH] E2: a sample stands for the step around it, so the stretch reaches half a step either side of the
		// first and the last sample that found smoke.
		if (!hitAny)
		{
			hitFirst = max(t - 0.5 * dt, 0.0);
			hitAny = true;
		}
		hitLast = t + 0.5 * dt;
#endif
		transmittance *= stepTransmittance;
		if (transmittance < 1.0 / 256.0)
			break;
	}
#if defined(SMOKE_SHARED)
	// [SHAREDMARCH] E2: the packed march both eyes carry from. Every distance is put in VIEW DEPTH (/ stepLen), which is
	// what the carry, the eye's own depth texture and the temporal resolve all speak in.
	//
	// BACK'S SIGN. A ray that still had smoke in its last step was STOPPED by something -- the scene, or the grid's own box --
	// and whatever lies past that stop is hidden from this view, so an eye that sees further is looking around an edge and the
	// carry must mark a hole. Back is then the stop itself rather than the last sample: half a step of slack there would make
	// every texel in the smoke look disoccluded. A ray whose smoke simply ran out first carries the negative of where it ran
	// out, and an eye that sees past that sees nothing more.
	bool sharedCut = hitAny && (hitLast + dt >= tOut);
	FragColor = SharedMarchPack(light, transmittance,
		hitAny ? hitFirst / stepLen : 0.0,
		hitAny ? (sharedCut ? tOut : -hitLast) / stepLen : 0.0,
		transmittance < 1.0 ? depthSum / max(1.0 - transmittance, 1e-6) / stepLen : 0.0,
		changeSum / max(reachSum, 1e-6));
#elif defined(SMOKE_SHARED_FILL)
	// [SHAREDMARCH] E2: the near shell this eye marched, put OVER what the carry left -- light + T x carried light, T x
	// carried T, which is the same 'over' the march itself does between two stretches of one ray. A hole marched the whole
	// ray, so there is nothing to put it over. The representative depth and the change level are combined by opacity, the
	// way the temporal march defines them, so the resolve reads the same kind of number it always did.
	float nearOpacity = 1.0 - transmittance;
	float nearDepth = transmittance < 1.0 ? depthSum / max(nearOpacity, 1e-6) / stepLen : 0.0;
	float nearChange = changeSum / max(reachSum, 1e-6);
	if (carriedHole)
	{
		FragColor = SmokeSharedOut(light, transmittance, nearDepth, nearChange);
	}
	else
	{
		float farOpacity = transmittance * (1.0 - carried.Transmittance);
		float weight = nearOpacity + farOpacity;
		FragColor = SmokeSharedOut(light + transmittance * carried.Light, transmittance * carried.Transmittance,
			weight > 1e-6 ? (nearDepth * nearOpacity + carried.Depth * farOpacity) / weight : 0.0,
			max(nearChange, carried.Change));
	}
#elif defined(SMOKE_TEMPORAL)
	// [SMOKE_TEMPORAL] The opacity shares sum to 1 - T, so the first is the mean distance of the smoke (/ stepLen: in view depth);
	// the second the share of the reachable smoke that changed.
	FragColor = vec4(light, SmokeTemporalPack(transmittance, depthSum / max(1.0 - transmittance, 1e-6) / stepLen, changeSum / max(reachSum, 1e-6)));
#else
	FragColor = vec4(light, transmittance);
#endif
}

#endif
