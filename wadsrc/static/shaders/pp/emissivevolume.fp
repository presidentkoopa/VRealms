
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D MarchDepthTexture;	// smokedepth.fp at this pass's resolution: the linear view depth to march to
layout(binding=1) uniform sampler2D EmissiveVolumeList;	// EMISSIVE_VOLUMES_DRAWN_MAX x EMISSIVE_VOLUME_TEXELS RGBA32F (hw_emissivevolumeframe.h)
layout(binding=2) uniform sampler3D EmissiveNoise;		// EMISSIVE_NOISE_SIZE^3, r and g smooth tileable value noise (linear, repeat)
// SMOKE_TRANSMITTANCE: this eye's smoke drew its transmittance curve (PPSmokeVolume::TransmittanceReady). Half resolution,
// read with the smoke composite's upsample, as volumetricbeam.fp reads them.
#if defined(SMOKE_TRANSMITTANCE)
layout(binding=3) uniform sampler2D SmokeMarchTexture;	// the smoke's blurred march: a = the whole ray's transmittance
layout(binding=4) uniform sampler2D SmokeDepthTexture;	// the depth each of its texels marched to
layout(binding=5) uniform sampler2D TransmittanceCurve;	// where each ray's optical depth reaches 0, 1/3, 2/3, all
#endif

// ============================================================================
// [EMISSIVEVOLUMES] EMISSIVE VOLUMES: THE MARCH ("Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" 2b-2e; the C++ is
// PPEmissiveVolumes, hw_postprocess.h; the numbers are hw_emissivevolumecore.h's).
//
// For each texel: the light the listed volumes along the view ray send toward the eye (rgb), and how much of what lies
// behind still shows through (a). Exactly (0, 0, 0, 1) wherever no volume's bounding sphere is met, so smokeblur.fp keeps
// the texel clear and smokecomposite.fp discards the pixel: everything outside a volume -- the lasers among it -- keeps its
// image bit for bit.
//
// THE RAY is smokemarch.fp's: TanHalfFov and ProjOffset in view space with z at -1, out through ViewToWorld; one unit along
// it is one unit of view depth, so the depth compares directly, and t is map units along the unit ray -- the smoke curve's
// own units. Positions are relative to this eye, GL world axes.
//
// THE VOLUMES, FAR TO NEAR (the list's order, sorted from the head once a frame). Each: an analytic ray/sphere chord against
// its bound, clipped to [0, the scene]; then StepCount x chord / diameter steps -- but at least one per unit of optical depth the
// gas can hold along the chord, at least 4 and at most 64 -- from the smoke's interleaved-gradient jitter. A step's density is the SHAPE FIELD (2b) -- the largest of a tapered round-ended body, the nearest petal of a
// crown and its two neighbours (so the cost does not grow with the petals), and a ring -- eroded by two reads of the baked
// noise flowing outward (billow) and changing in place (churn), with the hand's follow undone first. Its temperature is the
// core heat now x density ^ falloff, its colour the engine's fixed heat ramp x the volume's emission x density. Beer-Lambert
// with the volume's absorption: each step adds T x emission x (1 - e^(-sigma dt)) / sigma, which is emission x dt at no
// absorption. A volume composites OVER the farther ones: light = its light + light x its T.
//
// SMOKE_TRANSMITTANCE (2e): each step's light is dimmed by the haze between the eye and that step -- the smoke's curve, the
// same reader volumetricbeam.fp uses. Haze behind the volume never dims it (this pass runs after the smoke composite).
// ============================================================================

// [EMISSIVETILES] E5 ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E5; "Engine docs/EMISSIVE_TILES_E5_IMPL_NOTES.md"): TILE LISTS.
// With EMISSIVE_TILES each texel loops only the volumes its SCREEN_TILE_TEXELS x SCREEN_TILE_TEXELS tile lists -- bit i of the tile's
// mask (PPScreenTileMask, screentilemask.fp) is the list's volume i, whose widened screen bound touches the tile -- still in the
// list's order, and a tile that lists none is clear at once. Every volume a texel's ray can reach is listed (hw_screentiles.h), a
// volume that is not reached adds nothing, and no volume's work depends on the others: the texel gets the same light and
// transmittance as looping every volume. Without EMISSIVE_TILES this lump is the march it was.
#if defined(EMISSIVE_TILES)
#if defined(SMOKE_TRANSMITTANCE)
layout(binding=6) uniform sampler2D EmissiveTileMask;	// PPScreenTileMask's texture: a texel a tile, the mask a byte a channel
#else
layout(binding=3) uniform sampler2D EmissiveTileMask;
#endif
const int SCREEN_TILE_TEXELS = 16;		// hw_screentiles.h

// The mask of the tile holding this pass's texel: RGBA8 UNORM, byte 0 in r.
uint EmissiveTileMaskAt(ivec2 texel)
{
	uvec4 channels = uvec4(texelFetch(EmissiveTileMask, texel / SCREEN_TILE_TEXELS, 0) * 255.0 + 0.5);
	return channels.r | (channels.g << 8u) | (channels.b << 16u) | (channels.a << 24u);
}
#endif

// [SHAREDMARCH] E2 ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E2; "Engine docs/SHARED_MARCH_E2_IMPL_NOTES.md"): the volumes
// are marched once from a view halfway between the eyes (EMISSIVE_SHARED) and each eye carries that result into its own
// texels (sharedmarchwarp.fp); EMISSIVE_SHARED_FILL then marches the NEAR volumes -- the only ones the tile mask lists for
// it -- and puts them over the carry, or marches every volume where the carry left a hole. Both defines imply
// EMISSIVE_TILES, because the tile mask is what tells each pass which volumes are its own.
#if defined(EMISSIVE_SHARED_FILL)
#if defined(SMOKE_TRANSMITTANCE)
layout(binding=7) uniform sampler2D SharedMarchCarried;
#else
layout(binding=4) uniform sampler2D SharedMarchCarried;
#endif
#endif

#if defined(EMISSIVE_SHARED) || defined(EMISSIVE_SHARED_FILL)
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

const int EMISSIVE_VOLUMES_DRAWN_MAX = 32;		// hw_emissivevolumeframe.h
const int EVROW_BASE = 0;
const int EVROW_AXIS = 1;
const int EVROW_FRAME = 2;
const int EVROW_FOLLOW = 3;
const int EVROW_BODY = 4;
const int EVROW_PETALS = 5;
const int EVROW_PETALS2 = 6;
const int EVROW_RING = 7;
const int EVROW_NOISE = 8;
const int EVROW_NOISE2 = 9;
const int EVROW_EMISSION = 10;
const int EVROW_HEAT = 11;
const float EMISSIVE_NOISE_TILE_CELLS = 8.0;	// EMISSIVE_NOISE_SIZE / EMISSIVE_NOISE_LATTICE
const float EMISSIVE_TWO_PI = 6.28318530718;
const int EMISSIVE_STEPS_MAX = 64;				// r_emissivevolumes_steps' largest value

float InterleavedGradientNoise(vec2 pixel)
{
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// [LOOKS] THE HEAT RAMP, fixed in the engine so a hot look cannot go pink or green by accident
// ("Engine docs/GPU_PARTICLE_LOOKS_PLAN.md", guardrail 1): soot, deep red, orange, yellow,
// white-hot, and dimmer as it cools. Every key has red >= green >= blue, so every mix between
// keys does too. A definition's colour ramp multiplies it (vParticleColor), which is how plasma
// or BFG fire would keep their colour.
// [EMISSIVEVOLUMES] Copied from gpuparticles.fp token for token (a post-process lump has no #include): keep the two identical.
const vec3 kHeatRamp[5] = vec3[5](
	vec3(0.0, 0.0, 0.0),		// 0     soot: gives no light
	vec3(0.42, 0.045, 0.0),		// 0.25  deep red
	vec3(1.0, 0.32, 0.03),		// 0.5   orange
	vec3(1.0, 0.68, 0.22),		// 0.75  yellow
	vec3(1.0, 0.96, 0.86));		// 1     white-hot

vec3 ParticleHeatColor(float heat)
{
	float rampPosition = clamp(heat, 0.0, 1.0) * 4.0;
	int k = min(int(rampPosition), 3);
	return mix(kHeatRamp[k], kHeatRamp[k + 1], rampPosition - float(k));
}

#if defined(SMOKE_TRANSMITTANCE)
// [13e] THE TRANSMITTANCE CURVE, READ -- the same functions in smokemarch.fp, smokecomposite.fp, volumetricbeam.fp and
// [EMISSIVEVOLUMES] here; keep them identical. knots: the distances where a ray's optical depth reaches 0, 1/3, 2/3 and all of
// it (knots.w 0 = no smoke seen).
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

// This pixel's whole smoke transmittance and its curve, from the four half-resolution texels around it with the
// smoke composite's weights (smokecomposite.fp): bilinear share x closeness of the depth each texel marched to
// against this pixel's own depth z. Texels whose curve saw no smoke are left out of the curve's blend. 1 (and no
// curve) where all four are exactly clear.
float SmokeWholeTransmittance(float z, out vec4 knots)
{
	const vec4 clearTexel = vec4(0.0, 0.0, 0.0, 1.0);
	knots = vec4(0.0);

	ivec2 smokeSize = textureSize(SmokeMarchTexture, 0);
	vec2 at = TexCoord * vec2(smokeSize) - vec2(0.5);
	vec2 corner = floor(at);
	vec2 frac2 = at - corner;
	ivec2 cornerTexel = ivec2(corner);

	float sum = 0.0;
	float weightSum = 0.0;
	vec4 knotSum = vec4(0.0);
	float knotWeight = 0.0;
	bool anySmoke = false;
	for (int j = 0; j < 2; j++)
	{
		for (int i = 0; i < 2; i++)
		{
			ivec2 texel = clamp(cornerTexel + ivec2(i, j), ivec2(0), smokeSize - ivec2(1));
			vec4 smoke = texelFetch(SmokeMarchTexture, texel, 0);
			if (smoke != clearTexel)
				anySmoke = true;
			float marched = texelFetch(SmokeDepthTexture, texel, 0).r;
			float bilinear = (i == 0 ? 1.0 - frac2.x : frac2.x) * (j == 0 ? 1.0 - frac2.y : frac2.y) + 0.001;
			float relative = abs(marched - z) / max(min(marched, z), 1.0);
			float w = bilinear / (0.0004 + relative * relative);
			sum += smoke.a * w;
			weightSum += w;
			vec4 texelKnots = texelFetch(TransmittanceCurve, texel, 0);
			if (texelKnots.w > 0.0)
			{
				knotSum += texelKnots * w;
				knotWeight += w;
			}
		}
	}
	if (!anySmoke)
		return 1.0;
	if (knotWeight > 0.0)
		knots = knotSum / knotWeight;
	return clamp(sum / weightSum, 0.0, 1.0);
}
#endif

// One listed volume, as its column of the list holds it (hw_emissivevolumeframe.h, EEmissiveVolumeRow). base is eye-relative.
struct EmissiveVolume
{
	vec3 base;			// the root, eye-relative GL axes
	float bound;		// the bounding sphere's radius
	vec3 axis;
	float boundAlong;	// its centre along the axis from the base
	vec3 across;		// petal 0's direction across the axis
	float bodyLength;
	vec3 follow;		// the root's follow displacement
	float followTip;
	vec4 body;			// base radius, tip radius, softness, weight
	vec4 petals;		// count, length, width, weight
	vec4 petals2;		// cos spread, sin spread, jitter, the lag's axial reach
	vec4 ring;			// radius, thickness, weight, heat falloff
	vec4 grainOffset;	// the noise's offset (tiles), amount
	vec4 grain;			// cells per map unit, octaves, billow distance, churn phase
	vec4 emission;		// rgb emission, extinction per map unit
	vec4 heat;			// core heat now, shape seed, shape scale, 0
};

EmissiveVolume LoadVolume(int i, vec4 rowBase, vec4 rowAxis)
{
	EmissiveVolume v;
	vec4 rowFrame = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_FRAME), 0);
	vec4 rowFollow = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_FOLLOW), 0);
	v.base = ListOrigin + rowBase.xyz;
	v.bound = rowBase.w;
	v.axis = rowAxis.xyz;
	v.boundAlong = rowAxis.w;
	v.across = rowFrame.xyz;
	v.bodyLength = rowFrame.w;
	v.follow = rowFollow.xyz;
	v.followTip = rowFollow.w;
	v.body = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_BODY), 0);
	v.petals = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_PETALS), 0);
	v.petals2 = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_PETALS2), 0);
	v.ring = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_RING), 0);
	v.grainOffset = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_NOISE), 0);
	v.grain = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_NOISE2), 0);
	v.emission = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_EMISSION), 0);
	v.heat = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_HEAT), 0);
	return v;
}

// hw_emissivevolumecore.h's Hash and Share, in 32-bit unsigned arithmetic: the same numbers on the CPU and here.
uint EmissiveVolumeHash(uint x)
{
	x ^= x >> 16u; x *= 0x7feb352du;
	x ^= x >> 15u; x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

float EmissiveVolumeShare(uint seed, uint stream)
{
	return float(EmissiveVolumeHash(seed ^ EmissiveVolumeHash(stream * 0x85EBCA77u + 0x68bc21ebu)) >> 8u) / 16777216.0;
}

// The signed distance, in the plane of one axis, to a round cone from a sphere of r0 at axial 0 to a sphere of r1 at axial h
// (radial: the distance from that axis). A cone shorter than a hair, or one whose smaller sphere lies inside the larger, is
// its larger sphere.
float RoundConeDistance(float radial, float axial, float r0, float r1, float h)
{
	vec2 p = vec2(radial, axial);
	if (h < 1e-4)
		return length(p) - max(r0, r1);
	float b = (r0 - r1) / h;
	if (abs(b) >= 1.0)
		return r0 >= r1 ? length(p) - r0 : length(p - vec2(0.0, h)) - r1;
	float a = sqrt(1.0 - b * b);
	float k = dot(p, vec2(-b, a));
	if (k < 0.0)
		return length(p) - r0;
	if (k > a * h)
		return length(p - vec2(0.0, h)) - r1;
	return dot(p, vec2(a, b)) - r0;
}

// 0 on and outside a part's surface, rising smoothly to 1 at softWidth inside it.
float EdgeDensity(float distanceToSurface, float softWidth)
{
	float x = clamp(-distanceToSurface / softWidth, 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}

// Where eye-relative point p sits in the volume's own frame, the follow undone (hw_emissivevolumecore.h, Evaluate): a point at
// axial share a of the lag's reach was moved by follow x (1 - (1 - followTip) x clamp(a / reach, 0, 1)). That map is
// monotonic along the axis (the CPU keeps its slope at 1/2 or more), so it is undone exactly, piece by piece.
vec3 VolumeLocal(EmissiveVolume v, vec3 p)
{
	vec3 q = p - v.base;
	float axialSeen = dot(q, v.axis);
	float followAlong = dot(v.follow, v.axis);
	float reach = max(v.petals2.w, 1.0);
	float a = axialSeen - followAlong;
	if (a > 0.0)
	{
		a /= 1.0 - followAlong * (1.0 - v.followTip) / reach;
		if (a > reach)
			a = axialSeen - followAlong * v.followTip;
	}
	return q - v.follow * (1.0 - (1.0 - v.followTip) * clamp(a / reach, 0.0, 1.0));
}

// THE SHAPE FIELD (2b) at q in the volume's own frame: the largest of the body, the petals and the ring, each its weight x its
// edge. O(1): the body, at most three petals (the nearest to q's angle around the axis and its two neighbours), the ring.
float VolumeDensity(EmissiveVolume v, vec3 q)
{
	float axial = dot(q, v.axis);
	float radial = length(q - v.axis * axial);
	float softness = v.body.z;
	float density = 0.0;
	if (v.body.w > 0.0)
	{
		float d = RoundConeDistance(radial, axial, v.body.x, v.body.y, v.bodyLength);
		density = max(density, v.body.w * EdgeDensity(d, max(softness * max(v.body.x, v.body.y), 0.25)));
	}
	int count = int(v.petals.x + 0.5);
	if (count > 0 && v.petals.w > 0.0 && v.petals.z > 0.0)
	{
		vec3 across2 = cross(v.axis, v.across);
		float angle = atan(dot(q, across2), dot(q, v.across));
		if (angle < 0.0)
			angle += EMISSIVE_TWO_PI;
		float sector = EMISSIVE_TWO_PI / float(count);
		int nearest = int(floor(angle / sector + 0.5));
		if (nearest >= count)
			nearest -= count;
		int tries = min(count, 3);
		uint seed = uint(v.heat.y);
		for (int k = 0; k < tries; k++)
		{
			int petal = tries < 3 ? k : nearest + k - 1;
			if (petal < 0)
				petal += count;
			if (petal >= count)
				petal -= count;
			float turn = float(petal) * sector;
			vec3 direction = v.axis * v.petals2.x + (v.across * cos(turn) + across2 * sin(turn)) * v.petals2.y;
			float petalLength = v.petals.y * (1.0 + v.petals2.z * (2.0 * EmissiveVolumeShare(seed, 16u + uint(petal) * 2u) - 1.0));
			float petalWidth = v.petals.z * (1.0 + 0.5 * v.petals2.z * (2.0 * EmissiveVolumeShare(seed, 17u + uint(petal) * 2u) - 1.0));
			float petalAxial = dot(q, direction);
			float petalRadial = length(q - direction * petalAxial);
			float d = RoundConeDistance(petalRadial, petalAxial, petalWidth * 0.5, petalWidth * 0.1, petalLength);
			density = max(density, v.petals.w * EdgeDensity(d, max(softness * petalWidth * 0.5, 0.25)));
		}
	}
	if (v.ring.z > 0.0 && v.ring.y > 0.0)
	{
		float d = length(vec2(radial - v.ring.x, axial)) - v.ring.y;
		density = max(density, v.ring.z * EdgeDensity(d, max(softness * v.ring.y, 0.25)));
	}
	return density;
}

// The noise at q, 0..1: r at the sample's cells in the volume's own frame (so a volume that rides the gun carries its pattern),
// flowed outward by the billow and moved in place by the churn; with two octaves, g at 2.03 x them, 0.65 / 0.35.
float VolumeGrain(EmissiveVolume v, vec3 q)
{
	vec3 across2 = cross(v.axis, v.across);
	vec3 local = vec3(dot(q, v.axis), dot(q, v.across), dot(q, across2));
	local -= local / (length(local) + max(v.body.x, 1.0)) * v.grain.z;
	vec3 uvw = local * (v.grain.x / EMISSIVE_NOISE_TILE_CELLS) + v.grainOffset.xyz + vec3(0.13, 0.29, 0.41) * (v.grain.w / EMISSIVE_NOISE_TILE_CELLS);
	float first = texture(EmissiveNoise, uvw).r;
	if (v.grain.y < 1.5)
		return first;
	float second = texture(EmissiveNoise, uvw * 2.03 + vec3(0.37, 0.61, 0.17) - vec3(0.23, 0.11, 0.31) * (v.grain.w / EMISSIVE_NOISE_TILE_CELLS)).g;
	return first * 0.65 + second * 0.35;
}

void main()
{
	FragColor = vec4(0.0, 0.0, 0.0, 1.0);
#if defined(EMISSIVE_SHARED)
	// [SHAREDMARCH] E2: a texel of the shared march that meets no volume is the packed clear, not the plain one.
	FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, 0.0, 0.0);
#endif
#if defined(EMISSIVE_SHARED_FILL)
	// [SHAREDMARCH] E2: the fill needs this texel's ray before anything else, because what the carry left must be dimmed by
	// the haze in front of it even where no near volume is marched. The rectangle and the tile test come after, and each of
	// them leaves the carry standing.
	vec2 ndc = TexCoord * 2.0 - 1.0;
	vec3 viewRay = vec3((ndc + ProjOffset) * TanHalfFov, -1.0);
	vec3 worldRay = mat3(ViewToWorld) * viewRay;
	float stepLen = length(worldRay);
	if (stepLen < 1e-6)
		return;
	vec3 rd = worldRay / stepLen;
	float linearDepth = texelFetch(MarchDepthTexture, ivec2(gl_FragCoord.xy), 0).r;
	float sceneT = linearDepth * stepLen;
	vec4 smokeKnots = vec4(0.0);
	float smokeWholeT = 1.0;
	bool smokeDims = false;
	float smokeShareAtScene = 0.0;
	bool smokeRead = false;
	SharedMarchSample carried = SharedMarchUnpack(texelFetch(SharedMarchCarried, ivec2(gl_FragCoord.xy), 0));
	bool carriedHole = carried.Depth <= SHARED_MARCH_HOLE * 0.5;
	vec3 carriedLight = carriedHole ? vec3(0.0) : carried.Light;
	float carriedT = carriedHole ? 1.0 : carried.Transmittance;
#if defined(SMOKE_TRANSMITTANCE)
	// The haze between this eye and the carried light. The shared view has no transmittance curve of its own, so the shared
	// march is drawn undimmed and the dimming is applied here, per eye, at the depth the carried light sits at.
	if (!carriedHole && carried.Depth > 0.0)
	{
		smokeWholeT = SmokeWholeTransmittance(linearDepth, smokeKnots);
		smokeDims = smokeWholeT < 1.0;
		smokeShareAtScene = SmokeDepthShare(smokeKnots, sceneT);
		smokeRead = true;
		if (smokeDims)
			carriedLight *= SmokeTransmittanceTo(smokeKnots, carried.Depth * stepLen, smokeShareAtScene, smokeWholeT);
	}
#endif
	FragColor = vec4(carriedLight, carriedT);
	// A hole marches every volume this eye can see; otherwise only the ones its tile lists, which are the near ones.
	uint tileMask = carriedHole ? 0xFFFFFFFFu : EmissiveTileMaskAt(ivec2(gl_FragCoord.xy));
	if (VolumeCount <= 0 || tileMask == 0u)
		return;
	if (any(lessThan(TexCoord, RectMin)) || any(greaterThan(TexCoord, RectMax)))
		return;
#else
	if (VolumeCount <= 0 || any(lessThan(TexCoord, RectMin)) || any(greaterThan(TexCoord, RectMax)))
		return;

#if defined(EMISSIVE_TILES)
	// [EMISSIVETILES] E5: the volumes this texel's tile lists. None: clear, as a texel outside the rectangle is.
	uint tileMask = EmissiveTileMaskAt(ivec2(gl_FragCoord.xy));
	if (tileMask == 0u)
		return;
#endif

	vec2 ndc = TexCoord * 2.0 - 1.0;
	vec3 viewRay = vec3((ndc + ProjOffset) * TanHalfFov, -1.0);
	vec3 worldRay = mat3(ViewToWorld) * viewRay;
	float stepLen = length(worldRay);
	if (stepLen < 1e-6)
		return;
	vec3 rd = worldRay / stepLen;
	float linearDepth = texelFetch(MarchDepthTexture, ivec2(gl_FragCoord.xy), 0).r;
	float sceneT = linearDepth * stepLen;

#if defined(SMOKE_TRANSMITTANCE)
#if defined(EMISSIVE_TILES)
	// [EMISSIVETILES] E5: the texel's smoke reads wait for the first volume whose chord the ray meets (in the loop), so a texel of a
	// listed tile that no volume reaches reads no smoke. They are the same numbers whenever they are read, and only a volume's
	// steps use them.
	vec4 smokeKnots = vec4(0.0);
	float smokeWholeT = 1.0;
	bool smokeDims = false;
	float smokeShareAtScene = 0.0;
	bool smokeRead = false;
#else
	vec4 smokeKnots;
	float smokeWholeT = SmokeWholeTransmittance(linearDepth, smokeKnots);
	bool smokeDims = smokeWholeT < 1.0;
	float smokeShareAtScene = SmokeDepthShare(smokeKnots, sceneT);
#endif	// [EMISSIVETILES] E5: EMISSIVE_TILES
#endif
#endif	// [SHAREDMARCH] E2: EMISSIVE_SHARED_FILL

	float jitter = InterleavedGradientNoise(gl_FragCoord.xy);
	vec3 light = vec3(0.0);
	float transmittance = 1.0;
#if defined(EMISSIVE_SHARED)
	// [SHAREDMARCH] E2: the ends of the stretch the light came from, and a light-weighted depth inside it. The carry needs
	// all three: the ends to take the right share of the optical depth when this eye's own scene cuts the ray shorter, the
	// depth to find which texel of this pass holds the light an eye's ray is looking for.
	float hitFirst = 0.0;
	float hitLast = 0.0;
	bool hitAny = false;
	bool sharedCut = false;	// the scene cut a lit volume's chord short: see back's sign where this is packed
	float depthSum = 0.0;
	float weightSum = 0.0;
#endif
	int volumeCount = min(VolumeCount, EMISSIVE_VOLUMES_DRAWN_MAX);
	for (int i = 0; i < volumeCount; i++)
	{
#if defined(EMISSIVE_TILES)
		// [EMISSIVETILES] E5: only the volumes this tile lists, in the list's order; past its last listed volume there is nothing left.
		uint tileRest = tileMask >> uint(i);
		if (tileRest == 0u)
			break;
		if ((tileRest & 1u) == 0u)
			continue;
#endif
		vec4 rowBase = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_BASE), 0);
		vec4 rowAxis = texelFetch(EmissiveVolumeList, ivec2(i, EVROW_AXIS), 0);
		float bound = rowBase.w;
		if (!(bound > 0.0))
			continue;
		vec3 centre = ListOrigin + rowBase.xyz + rowAxis.xyz * rowAxis.w;
		float middle = dot(rd, centre);
		float disc = middle * middle - dot(centre, centre) + bound * bound;
		if (disc <= 0.0)
			continue;
		float halfChord = sqrt(disc);
		float t0 = max(middle - halfChord, 0.0);
		float t1 = min(middle + halfChord, sceneT);
		if (t1 <= t0)
			continue;

#if defined(EMISSIVE_TILES) && defined(SMOKE_TRANSMITTANCE)
		// [EMISSIVETILES] E5: the smoke reads, at the first volume whose chord this ray meets.
		if (!smokeRead)
		{
			smokeWholeT = SmokeWholeTransmittance(linearDepth, smokeKnots);
			smokeDims = smokeWholeT < 1.0;
			smokeShareAtScene = SmokeDepthShare(smokeKnots, sceneT);
			smokeRead = true;
		}
#endif
		EmissiveVolume v = LoadVolume(i, rowBase, rowAxis);
		// StepCount across the whole diameter, fewer for a shorter chord; and at least one step for each unit of optical depth the gas
		// can have along the chord (a sooty volume's front is where its light comes from), never more than 64.
		float densest = max(max(v.body.w, v.petals.w), v.ring.z);
		int steps = clamp(max(int(float(StepCount) * (t1 - t0) / (2.0 * bound) + 0.5), int(ceil(v.emission.w * densest * (t1 - t0)))), 4, EMISSIVE_STEPS_MAX);
		float dt = (t1 - t0) / float(steps);
		vec3 volumeLight = vec3(0.0);
		float volumeT = 1.0;
#if defined(EMISSIVE_SHARED)
		float volumeDepthSum = 0.0;	// [SHAREDMARCH] E2: this volume's own, composited with the rest below
		float volumeWeightSum = 0.0;
#endif
		for (int s = 0; s < steps; s++)
		{
			float t = t0 + (float(s) + jitter) * dt;
			vec3 q = VolumeLocal(v, rd * t);
			float density = VolumeDensity(v, q);
			if (!(density > 0.0))
				continue;
			if (v.grainOffset.w > 0.0)
			{
				float amount = v.grainOffset.w;
				density = clamp((density - amount * (1.0 - VolumeGrain(v, q))) / max(1.0 - 0.5 * amount, 0.5), 0.0, 1.0);
				if (!(density > 0.0))
					continue;
			}
			vec3 emitted = ParticleHeatColor(v.heat.x * pow(density, v.ring.w)) * v.emission.rgb * density;
			float extinction = v.emission.w * density;
			float stepT = exp(-extinction * dt);
			float path = extinction > 1e-7 ? (1.0 - stepT) / extinction : dt;
#if defined(SMOKE_TRANSMITTANCE)
			if (smokeDims)
				emitted *= SmokeTransmittanceTo(smokeKnots, t, smokeShareAtScene, smokeWholeT);
#endif
#if defined(EMISSIVE_SHARED)
			// [SHAREDMARCH] E2: the same weight the light is added with, so the depth below is the light-weighted mean of where
			// this volume's light actually comes from rather than the middle of its chord.
			float stepWeight = dot(volumeT * emitted * path, vec3(1.0));
			volumeDepthSum += stepWeight * t;
			volumeWeightSum += stepWeight;
#endif
			volumeLight += volumeT * emitted * path;
			volumeT *= stepT;
			if (volumeT < 1.0 / 256.0)
				break;
		}
#if defined(EMISSIVE_SHARED)
		// [SHAREDMARCH] E2: the depth and its weight composite exactly as the light does (this volume's own, then everything
		// behind it dimmed by this one), so the two stay the same average. The stretch is the union of the chords that lit.
		if (volumeWeightSum > 0.0)
		{
			hitFirst = hitAny ? min(hitFirst, t0) : t0;
			hitLast = hitAny ? max(hitLast, t1) : t1;
			hitAny = true;
			sharedCut = sharedCut || (t1 >= sceneT - 1e-3);
		}
		depthSum = volumeDepthSum + depthSum * volumeT;
		weightSum = volumeWeightSum + weightSum * volumeT;
#endif
		light = volumeLight + light * volumeT;
		transmittance *= volumeT;
	}
#if defined(EMISSIVE_SHARED)
	// [SHAREDMARCH] E2: the packed march both eyes carry from, every distance in VIEW DEPTH (/ stepLen). Emissive volumes
	// live two to ten frames and keep no history, so the change field is 0.
	// BACK'S SIGN: positive when the scene cut a lit volume's chord short, so an eye that sees past that stop is looking
	// around an edge at a volume this march never reached and the carry marks a hole; negative when every lit chord ended
	// on its own. Only the magnitude is the depth.
	FragColor = SharedMarchPack(light, transmittance,
		hitAny ? hitFirst / stepLen : 0.0,
		hitAny ? (sharedCut ? sceneT : -hitLast) / stepLen : 0.0,
		weightSum > 1e-9 ? depthSum / weightSum / stepLen : 0.0,
		0.0);
#elif defined(EMISSIVE_SHARED_FILL)
	// [SHAREDMARCH] E2: the near volumes this eye marched, put OVER what the carry left -- the same 'over' the volume loop
	// itself does, and right because a near volume is nearer than every carried one by construction. A hole marched every
	// volume, so there is nothing to put it over.
	if (carriedHole)
		FragColor = vec4(light, transmittance);
	else
		FragColor = vec4(light + transmittance * carriedLight, transmittance * carriedT);
#else
	FragColor = vec4(light, transmittance);
#endif
}
