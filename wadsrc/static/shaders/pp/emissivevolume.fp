
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
	if (VolumeCount <= 0 || any(lessThan(TexCoord, RectMin)) || any(greaterThan(TexCoord, RectMax)))
		return;

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
	vec4 smokeKnots;
	float smokeWholeT = SmokeWholeTransmittance(linearDepth, smokeKnots);
	bool smokeDims = smokeWholeT < 1.0;
	float smokeShareAtScene = SmokeDepthShare(smokeKnots, sceneT);
#endif

	float jitter = InterleavedGradientNoise(gl_FragCoord.xy);
	vec3 light = vec3(0.0);
	float transmittance = 1.0;
	int volumeCount = min(VolumeCount, EMISSIVE_VOLUMES_DRAWN_MAX);
	for (int i = 0; i < volumeCount; i++)
	{
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

		EmissiveVolume v = LoadVolume(i, rowBase, rowAxis);
		// StepCount across the whole diameter, fewer for a shorter chord; and at least one step for each unit of optical depth the gas
		// can have along the chord (a sooty volume's front is where its light comes from), never more than 64.
		float densest = max(max(v.body.w, v.petals.w), v.ring.z);
		int steps = clamp(max(int(float(StepCount) * (t1 - t0) / (2.0 * bound) + 0.5), int(ceil(v.emission.w * densest * (t1 - t0)))), 4, EMISSIVE_STEPS_MAX);
		float dt = (t1 - t0) / float(steps);
		vec3 volumeLight = vec3(0.0);
		float volumeT = 1.0;
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
			volumeLight += volumeT * emitted * path;
			volumeT *= stepT;
			if (volumeT < 1.0 / 256.0)
				break;
		}
		light = volumeLight + light * volumeT;
		transmittance *= volumeT;
	}
	FragColor = vec4(light, transmittance);
}
