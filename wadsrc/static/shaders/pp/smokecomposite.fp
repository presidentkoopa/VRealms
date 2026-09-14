
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SmokeTexture;		// the blurred march, half resolution: rgb light, a transmittance
layout(binding=1) uniform sampler2D SmokeDepthTexture;	// smokedepth.fp: the depth each half-resolution texel marched to
// MULTISAMPLE: the multisampled scene depth, read with texelFetch (as smokedepth.fp reads it).
#if defined(MULTISAMPLE)
layout(binding=2) uniform sampler2DMS DepthTexture;
#else
layout(binding=2) uniform sampler2D DepthTexture;
#endif
// [13e] SMOKE_BEAMS: this frame has beam lines in the smoke (PPSmokeVolume).
#if defined(SMOKE_BEAMS)
layout(binding=3) uniform sampler2D BeamScatterTexture;	// half resolution: rgb the light the beams scatter in the smoke
layout(binding=4) uniform sampler2D TransmittanceCurve;	// half resolution: where each ray's optical depth reaches 0, 1/3, 2/3, all
layout(binding=5) uniform sampler2D SmokeBeamList;		// SMOKE_BEAMS_MAX x 4 RGBA32F texels (hw_framecompute.h, SmokeBeamRecord)
#endif

// ============================================================================
// [SMOKEVOLUME] THE SMOKE VOLUME'S DRAWING, PASS 4 OF 4: UP TO FULL RESOLUTION AND ONTO THE IMAGE.
// ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c; the C++ is PPSmokeVolume.)
//
// Drawn over the scene viewport onto the current image with the PREMULTIPLIED blend
// (PPRenderState::SetPremultipliedAlphaBlend). The output is (light, 1 - T), so
//     image = light + image x T
// which is the composite "Engine docs/EMISSIVE_BLOOM_PLAN.md" requires of 13c: linear, premultiplied,
// the transmittance kept per pixel. Nothing here reads the image back.
//
// THE UPSAMPLE takes the four half-resolution texels around this pixel. Each is weighted by its
// bilinear share and by how close the depth it marched to is to this pixel's own depth. A pixel on a
// hand takes the march that stopped at the hand, and a pixel on the wall behind it takes the march that
// went on to the wall (smokedepth.fp gives every 2x2 neighbourhood both).
//
// A pixel whose four texels are all exactly clear is DISCARDED, so it is left exactly as it was. That
// is what keeps everything outside the smoke, lasers included, the image it was.
//
// LIGHT_MASK_CARRY (hw_postprocess.h, PPLightMask): the same weights and the same T, onto the light
// mask with the same blend, adding no light of either class. Every mask amount is therefore dimmed by
// exactly the T the colour is. The emissive air light E4 counts and the pinned beam scatter 13e adds
// are to go into its r and g.
//
// [13e] SMOKE_BEAMS ("Engine docs/SMOKE_13E_IMPL_NOTES.md"). Beams write no depth, so the scene's beam glow on
// this pixel was dimmed by ALL the haze on its ray (image x T), including haze behind the beam. Two more
// terms join the light, both beam light and both linear, so the composite is still scene x T + inscatter:
//   + the light the beams scatter in the smoke (smokemarch.fp SMOKE_BEAM_SCATTER), upsampled as the smoke is
//   + each beam's own air glow, re-evaluated here with main.fp's BeamAirGlow maths, x (T to the beam - T)
//     (with BeamDepth): what the haze behind the beam took, given back. T to the beam comes from the
//     transmittance curve, blended over the same four texels with the same weights.
// The carry writes both into g, the pinned class they are, so a pure beam pixel's pinned amount becomes its
// glow x T to the beam, exactly the share pinned bloom reads.
// ============================================================================

float LinearDepthAt(vec2 uv)
{
#if defined(MULTISAMPLE)
	ivec2 depthSize = textureSize(DepthTexture);
#else
	ivec2 depthSize = textureSize(DepthTexture, 0);
#endif
	ivec2 depthTexel = clamp(ivec2(uv * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
	float rawDepth = texelFetch(DepthTexture, depthTexel, 0).x;
	return 1.0 / (clamp(rawDepth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
}

#if defined(SMOKE_BEAMS)

const int SMOKE_BEAMS_MAX = 16;	// hw_framecompute.h

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

// [13e] One listed beam's glow seen in the air along this pixel's ray, exactly as main.fp's BeamAirGlow adds it for a
// surface fragDist away (the eye at the origin, eye-relative GL axes), and how far along the ray the beam passes
// closest (alongRay). Scroll from the scene's values (BeamScrollSpeed, BeamScrollDepth, BeamTimer), taper, flare, halo
// and air glow from the beam's own look.
vec3 BeamAirGlowAt(int i, vec3 dir, float fragDist, out float alongRay)
{
	alongRay = 0.0;
	vec4 rowA = texelFetch(SmokeBeamList, ivec2(i, 0), 0);
	vec4 rowB = texelFetch(SmokeBeamList, ivec2(i, 1), 0);
	vec4 colour = texelFetch(SmokeBeamList, ivec2(i, 2), 0);
	vec4 look = texelFetch(SmokeBeamList, ivec2(i, 3), 0);
	if (look.x <= 0.0)
		return vec3(0.0);

	vec3 a = BoxMin + rowA.xyz;
	vec3 b = BoxMin + rowB.xyz;
	float thick = max(rowA.w, 0.01);
	float soft = max(rowB.w, 0.01);

	vec3 mid = (a + b) * 0.5;
	float cull = length(b - a) * 0.5 + thick + soft * 6.0 + 1.0;
	float alongMid = clamp(dot(mid, dir), 0.0, fragDist);
	vec3 perp = mid - dir * alongMid;
	if (dot(perp, perp) > cull * cull)
		return vec3(0.0);

	vec3 v = b - a;
	vec3 w = -a;

	float bb = dot(dir, v);
	float cc = dot(v, v);
	float dd = dot(dir, w);
	float ee = dot(v, w);
	float den = cc - bb * bb;

	float sc;
	float tc;
	if (abs(den) < 0.0001)
	{
		sc = -dd;
		tc = 0.0;
	}
	else
	{
		sc = (bb * ee - cc * dd) / den;
		tc = (bb * -dd + ee) / den;
	}
	sc = clamp(sc, 0.0, fragDist);
	tc = clamp(tc, 0.0, 1.0);

	float dist = length((dir * sc) - (a + v * tc));

	float bw = mix(1.0 - look.z, 1.0, tc);
	thick *= bw;
	soft *= bw;

	float bright = 1.0;
	if (BeamScrollDepth > 0.0)
	{
		float alongLine = tc * length(v);
		float s = sin(alongLine * 0.06 - BeamTimer * BeamScrollSpeed);
		bright *= 1.0 + BeamScrollDepth * s;
	}
	if (look.w > 0.0)
		bright += look.w * pow(clamp(tc, 0.0, 1.0), 8.0);

	float core = 1.0 - smoothstep(thick * 0.5, thick + soft, dist);
	float halo = 1.0 - smoothstep(thick, thick + soft * 6.0 + 1.0, dist);

	alongRay = sc;
	return colour.rgb * (core * 1.6 + halo * look.y) * colour.w * look.x * bright;
}

#endif

void main()
{
	const vec4 clearTexel = vec4(0.0, 0.0, 0.0, 1.0);

	ivec2 smokeSize = textureSize(SmokeTexture, 0);
	vec2 at = TexCoord * vec2(smokeSize) - vec2(0.5);
	vec2 corner = floor(at);
	vec2 along = at - corner;
	ivec2 cornerTexel = ivec2(corner);

	float z = LinearDepthAt(SceneOffset + TexCoord * SceneScale);

	vec4 sum = vec4(0.0);
	float weightSum = 0.0;
	bool anySmoke = false;
#if defined(SMOKE_BEAMS)
	vec3 scatterSum = vec3(0.0);
	vec4 knotSum = vec4(0.0);
	float knotWeight = 0.0;
#endif
	for (int j = 0; j < 2; j++)
	{
		for (int i = 0; i < 2; i++)
		{
			ivec2 texel = clamp(cornerTexel + ivec2(i, j), ivec2(0), smokeSize - ivec2(1));
			vec4 smoke = texelFetch(SmokeTexture, texel, 0);
			if (smoke != clearTexel)
				anySmoke = true;
			float marched = texelFetch(SmokeDepthTexture, texel, 0).r;
			float bilinear = (i == 0 ? 1.0 - along.x : along.x) * (j == 0 ? 1.0 - along.y : along.y) + 0.001;
			float relative = abs(marched - z) / max(min(marched, z), 1.0);
			float w = bilinear / (0.0004 + relative * relative);
			sum += smoke * w;
			weightSum += w;
#if defined(SMOKE_BEAMS)
			vec3 scattered = texelFetch(BeamScatterTexture, texel, 0).rgb;
			if (scattered != vec3(0.0))
				anySmoke = true;
			scatterSum += scattered * w;
			vec4 knots = texelFetch(TransmittanceCurve, texel, 0);
			if (knots.w > 0.0)
			{
				knotSum += knots * w;
				knotWeight += w;
			}
#endif
		}
	}
	if (!anySmoke)
		discard;

	vec4 blended = sum / weightSum;
	float opacity = clamp(1.0 - blended.a, 0.0, 1.0);
#if defined(SMOKE_BEAMS)
	// [13e] The light the beams scatter, upsampled as the smoke is.
	vec3 beamLight = max(scatterSum / weightSum, vec3(0.0));
	// [13e] And each beam's own glow: the scene dimmed it by all of this ray's haze; give back what the haze behind the
	// beam took (T to the beam - T, never below 0: haze only ever dims further along the ray).
	if (BeamDepth != 0 && BeamCount > 0 && knotWeight > 0.0 && opacity > 0.0)
	{
		vec2 ndc = TexCoord * 2.0 - 1.0;
		vec3 viewRay = vec3((ndc + ProjOffset) * TanHalfFov, -1.0);
		vec3 worldRay = mat3(ViewToWorld) * viewRay;
		float stepLen = length(worldRay);
		if (stepLen > 1e-6)
		{
			vec3 rd = worldRay / stepLen;
			float fragDist = z * stepLen;
			float wholeT = 1.0 - opacity;
			vec4 knots = knotSum / knotWeight;
			float shareAtScene = SmokeDepthShare(knots, fragDist);
			int beamCount = min(BeamCount, SMOKE_BEAMS_MAX);
			for (int i = 0; i < beamCount; i++)
			{
				float alongRay;
				vec3 glow = BeamAirGlowAt(i, rd, fragDist, alongRay);
				if (glow != vec3(0.0))
					beamLight += glow * max(SmokeTransmittanceTo(knots, alongRay, shareAtScene, wholeT) - wholeT, 0.0);
			}
		}
	}
  #if defined(LIGHT_MASK_CARRY)
	FragColor = vec4(0.0, beamLight.r + beamLight.g + beamLight.b, 0.0, opacity);
  #else
	FragColor = vec4(max(blended.rgb, vec3(0.0)) + beamLight, opacity);
  #endif
#else
#if defined(LIGHT_MASK_CARRY)
	FragColor = vec4(0.0, 0.0, 0.0, opacity);
#else
	FragColor = vec4(max(blended.rgb, vec3(0.0)), opacity);
#endif
#endif
}
