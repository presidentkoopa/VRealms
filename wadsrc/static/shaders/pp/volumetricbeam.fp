
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// MULTISAMPLE is defined for the BeamMS variant (hw_postprocess.h), picked when
// gl_multisample > 1. With MSAA on, the scene depth is a multisampled texture,
// and reading that through a plain sampler2D reads as 0 -- which linearises to
// znear, pins the march to a few units and the beam never draws. Same split
// lineardepth.fp has always had, for the same reason.
#if defined(MULTISAMPLE)
layout(binding=0) uniform sampler2DMS DepthTexture;
#else
layout(binding=0) uniform sampler2D DepthTexture;
#endif
// [13e] SMOKE_TRANSMITTANCE: this eye drew smoke with its transmittance curve and r_smoke_cones_depth is on
// (PPSmokeVolume::ConesDimmedByHaze). All three at half resolution, read with the smoke composite's upsample.
#if defined(SMOKE_TRANSMITTANCE)
layout(binding=1) uniform sampler2D SmokeMarchTexture;	// the smoke's blurred march: a = the whole ray's transmittance
layout(binding=2) uniform sampler2D SmokeDepthTexture;	// the depth each of its texels marched to
layout(binding=3) uniform sampler2D TransmittanceCurve;	// where each ray's optical depth reaches 0, 1/3, 2/3, all
#endif

// ============================================================================
// [BB] Volumetric flashlight beam.
//
// Everything else in this engine lights SURFACES. This lights the air: the
// cone is drawn where nothing is, so you see the beam itself rather than only
// the disc it lands on. That is the whole point of it, and it is why this has
// to be a postprocess pass -- there is no geometry to hang it on.
//
// Working in VIEW space rather than world space is deliberate. The ray for a
// pixel is trivial there (it starts at the origin), and each eye resolves its
// own view matrix on the CPU, so stereo and portals come out right without
// this shader knowing either exists.
//
// The march is bounded by an analytic ray/cone intersection first. Without
// that, every pixel on screen would march the full ray even when the cone
// covers a tenth of the view -- the difference between a beam you can afford
// and one you cannot.
// ============================================================================

// ---------------------------------------------------------------------------
// Value noise, 3D. Cheap enough to afford once per march step.
// ---------------------------------------------------------------------------

float hash13(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float valueNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);   // smoothstep, so cells blend instead of blocking

	return mix(
		mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
		    mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
		mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
		    mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}

// Two octaves. One reads as smooth blobs; two gives the finer grain that
// makes it look like motes rather than fog.
float dustNoise(vec3 p)
{
	return valueNoise(p) * 0.65 + valueNoise(p * 2.7) * 0.35;
}

#if defined(SMOKE_TRANSMITTANCE)
// ---------------------------------------------------------------------------
// [13e] THE CONE IN SMOKE ("Engine docs/SMOKE_13E_IMPL_NOTES.md", G2).
//
// This pass runs after the smoke composite, so haze in front of the cone never dimmed it. Each step's glow is
// now dimmed by the transmittance from the eye to that step, read from the smoke's transmittance curve. The
// cone's glow is the air's dust and the smoke's own glow from the same light is 13d's light grid: two things
// scattering the same light add, and dimmed like this the dust's glow from deep inside thick haze fades away
// (it can be no more than glow / extinction), so the two never double up where the haze is thick.
// ---------------------------------------------------------------------------

// THE TRANSMITTANCE CURVE, READ -- the same two functions in smokemarch.fp, smokecomposite.fp and
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

void main()
{
	// View-space ray for this pixel. Origin is the eye, at (0,0,0).
	vec2 ndc = TexCoord * 2.0 - 1.0;

	// OFF-CENTRE FRUSTUMS. A headset eye's projection is asymmetric: m[8] and
	// m[9] are non-zero (vk_openxrdevice.cpp builds them from tanLeft/Right/
	// Up/Down), and they are opposite in the two eyes. Solving the projection
	// for a view point at z = -1 gives ndc = m0*x - m8, so x = (ndc + m8) / m0.
	// Rebuilding with ndc * TanHalfFov alone shifted each eye's rays sideways
	// in opposite directions -- the cone sat at the wrong stereo depth and did
	// not line up with the depth buffer it clips against. ProjOffset is
	// (m[8], m[9]), zero on a symmetric flat-screen projection, so the flat
	// case is unchanged. Holds for a Y-flipped matrix too: the flip negates
	// m[5] and m[9] together.
	vec3 rayDir = normalize(vec3((ndc + ProjOffset) * TanHalfFov, -1.0));

	// Scene depth for this pixel: how far along the ray the world is. The
	// beam must stop there, or it would shine through walls.
	//
	// THE SAMPLE IS NOT A DISTANCE. It is a nonlinear 0..1 depth-buffer value,
	// and this used to clamp the march against it directly as though it were
	// view-space map units -- so any geometry at all in front of the camera
	// capped tMax at under one unit and the integral covered nothing. Convert
	// it the way lineardepth.fp does, then turn the along-Z distance into a
	// distance along THIS ray, which is what tMin/tMax are measured in.
	//
	// SAMPLED INSIDE THE SCENE VIEWPORT. This pass draws over mSceneViewport,
	// so TexCoord runs 0..1 across the 3D view only, while the depth texture
	// covers the whole screen buffer. With a status bar or a reduced screen
	// size the two differ, and reading depth at raw TexCoord took it from the
	// wrong texels: the beam cut off early against walls or shone through them.
	// Same Offset + TexCoord * Scale that lineardepth.fp and the bloom extract
	// use; SceneScale/SceneOffset are (1,1)/(0,0) when the view fills the screen.
	vec2 depthUV = SceneOffset + TexCoord * SceneScale;
#if defined(MULTISAMPLE)
	// Sample 0, as the SSAO linear-depth pass does with SampleIndex 0. A
	// multisampled texture has no filtering; fetch the texel directly.
	ivec2 depthSize = textureSize(DepthTexture);
	ivec2 depthTexel = clamp(ivec2(depthUV * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
	float rawDepth = texelFetch(DepthTexture, depthTexel, 0).x;
#else
	float rawDepth = texture(DepthTexture, depthUV).x;
#endif
	float linearZ = 1.0 / (clamp(rawDepth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
	float sceneDepth = linearZ / max(-rayDir.z, 1e-4);

	// --- analytic ray/cone intersection --------------------------------
	// Bounds the march to the segment that can possibly be lit.
	vec3 co = -BeamPos;                  // eye relative to the cone apex
	float cosT = CosOuter;
	float cos2 = cosT * cosT;

	float dv = dot(rayDir, BeamDir);
	float cv = dot(co, BeamDir);

	// f(t) = a t^2 + b t + c is >= 0 where the ray point is inside the
	// INFINITE DOUBLE cone -- the lit forward nappe AND its mirror image
	// behind the apex. h(t) = dv t + cv is the point's distance along the beam
	// axis, positive only on the forward nappe. See the note below.
	float a = dv * dv - cos2;
	float b = 2.0 * (dv * cv - dot(rayDir, co) * cos2);
	float c = cv * cv - dot(co, co) * cos2;

	// THE CONE'S LENGTH IS MEASURED FROM THE APEX, not from the eye, so bound
	// the march by the sphere of radius BeamLength around the apex rather than
	// by t <= BeamLength. With the apex at the eye the two are the same; with
	// the apex on a hand, or behind the camera, only the sphere is right.
	float rb = dot(rayDir, BeamPos);
	float sphereDisc = rb * rb - dot(BeamPos, BeamPos) + BeamLength * BeamLength;
	if (sphereDisc <= 0.0) { FragColor = vec4(0.0); return; }
	float sphereSq = sqrt(sphereDisc);
	float reachFar = rb + sphereSq;
	if (reachFar <= 0.0) { FragColor = vec4(0.0); return; }
	float reachNear = max(rb - sphereSq, 0.0);

	const float FAR_T = 1.0e30;
	float tMin = 0.0;
	float tMax = 0.0;

	// ---------------------------------------------------------------------
	// THE APEX AT THE EYE NEEDS NO QUADRATIC.
	//
	// A torch on the head puts the cone's apex within a hair of the view
	// origin. Then co is zero, so b and c are zero, both roots are zero, and a
	// general solve returns an empty segment -- which is how this pass once
	// drew black for every pixel in that configuration. With the apex at the
	// eye the ray either lies inside the cone or it does not -- one dot
	// product -- and if it does, the lit stretch is the whole ray out to
	// whatever stops it.
	//
	// (This used to say AttackPos IS the eye. It is not. In VR AttackPos is
	// the CONTROLLER, written per frame in hw_vrmodes.cpp / vk_openxrdevice.cpp;
	// on a flat screen it is PosAtZ(shootz), the shooting height, not viewz.
	// So a torch read from AttackPos is usually NOT in this branch -- it is in
	// the general one below, which is why that one has to be right.)
	// ---------------------------------------------------------------------
	if (dot(BeamPos, BeamPos) < 1.0)
	{
		if (dv <= cosT) { FragColor = vec4(0.0); return; }
		tMin = 0.0;
		tMax = FAR_T;
	}
	else
	{
		// -----------------------------------------------------------------
		// ONLY THE FORWARD NAPPE IS LIT, AND THE LIT PART IS ONE INTERVAL.
		//
		// The quadratic solves the DOUBLE cone. The old code took [t0, t1]
		// between its two roots, which is right only when the ray crosses the
		// forward nappe twice (a < 0). When the ray looks along the beam
		// (a > 0), one root is on the mirror nappe BEHIND the apex, and the
		// stretch between the roots is the gap outside the cone. Measured with
		// the apex 4 units below the eye, beam forward: the shader delivered
		// 1.67 of 82.87 units of light; a VR hand torch got about 6%. The far
		// body of the beam -- the part you look along -- was black.
		//
		// The forward nappe (half-angle < 90, clamped in SetVolumetricBeam) is
		// a CONVEX set, so a ray meets it in exactly one interval, and that
		// interval is fixed by three facts:
		//   startsInside  the eye itself is inside the forward nappe
		//   endsInside    the ray's direction is within the cone angle, so far
		//                 enough out it is inside for good
		//   crossings     roots with t > 0 whose point has h(t) > 0; roots on
		//                 the mirror nappe are discarded
		// in -> in    [0, far)            in -> out   [0, first crossing]
		// out -> in   [last crossing, far) out -> out [first, second] or nothing
		// -----------------------------------------------------------------
		bool startsInside = (cv > 0.0) && (c >= 0.0);
		bool endsInside = dv > cosT;

		float cross0 = 0.0;
		float cross1 = 0.0;
		int crossings = 0;

		if (abs(a) < 1e-6)
		{
			// Ray parallel to the cone surface: f is linear, one root at most.
			if (abs(b) > 1e-6)
			{
				float t = -c / b;
				if (t > 0.0 && dv * t + cv > 0.0) { cross0 = t; crossings = 1; }
			}
		}
		else
		{
			float disc = b * b - 4.0 * a * c;
			if (disc >= 0.0)
			{
				// Numerically stable roots: the textbook (-b +- sq) / 2a loses
				// the small root to cancellation when b dominates, which is
				// exactly the grazing case at the edge of the cone.
				float sq = sqrt(disc);
				float q = -0.5 * (b + (b >= 0.0 ? sq : -sq));
				float r0 = q / a;
				float r1 = (abs(q) > 1e-12) ? c / q : r0;
				float lo = min(r0, r1);
				float hi = max(r0, r1);
				if (lo > 0.0 && dv * lo + cv > 0.0) { cross0 = lo; crossings = 1; }
				if (hi > 0.0 && dv * hi + cv > 0.0)
				{
					if (crossings == 0) cross0 = hi; else cross1 = hi;
					crossings++;
				}
			}
		}

		if (startsInside && endsInside)
		{
			tMin = 0.0;
			tMax = FAR_T;
		}
		else if (startsInside)
		{
			tMin = 0.0;
			tMax = (crossings > 0) ? cross0 : FAR_T;
		}
		else if (endsInside)
		{
			// Convexity says one crossing; take the last if rounding found two.
			tMin = (crossings > 1) ? cross1 : ((crossings > 0) ? cross0 : 0.0);
			tMax = FAR_T;
		}
		else
		{
			if (crossings < 2) { FragColor = vec4(0.0); return; }
			tMin = cross0;
			tMax = cross1;
		}
	}

	tMin = max(tMin, reachNear);
	tMax = min(tMax, min(reachFar, sceneDepth));
	if (tMax <= tMin) { FragColor = vec4(0.0); return; }

	// --- march ----------------------------------------------------------
	// Jittered start, so banding across the cone turns into fine noise the
	// eye reads as haze rather than as visible steps. Interleaved gradient
	// noise: one cheap expression, no texture lookup.
	float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

#if defined(SMOKE_TRANSMITTANCE)
	// [13e] The smoke on this pixel's ray. The smoke measures distance along the ray in map units and this pass in
	// view units; the view matrix carries the pixel stretch, so the two differ by this ray's own scale.
	vec4 smokeKnots;
	float smokeWholeT = SmokeWholeTransmittance(linearZ, smokeKnots);
	bool smokeDims = smokeWholeT < 1.0;
	float smokeScale = length(mat3(ViewToWorld) * rayDir);
	float smokeShareAtScene = SmokeDepthShare(smokeKnots, sceneDepth * smokeScale);
#endif

	int steps = StepCount;
	float dt = (tMax - tMin) / float(steps);
	float t = tMin + dt * jitter;

	float accum = 0.0;
	for (int i = 0; i < 64; i++)
	{
		if (i >= steps) break;

		vec3 p = rayDir * t;
		vec3 toP = p - BeamPos;
		float dist = length(toP);

		if (dist > 0.0001)
		{
			float ct = dot(toP / dist, BeamDir);

			// Inside the cone at all?
			if (ct > cosT)
			{
				// Radial: full brightness inside the inner angle, easing out
				// to nothing at the outer. This is the soft edge of the beam.
				float radial = smoothstep(cosT, CosInner, ct);

				// Axial: fades along the length so the beam dies out instead
				// of ending. Falloff shapes the curve -- 1 linear, higher
				// concentrates the light near the lens.
				float axial = 1.0 - clamp(dist / BeamLength, 0.0, 1.0);
				axial = pow(axial, Falloff);

				float contrib = radial * axial;

				// Dust. Sampled in WORLD space, not beam space, and that is
				// the whole trick: dust hangs in the room, it does not travel
				// with the torch. Sample it relative to the beam and the
				// motes slide along with the cone as you sweep, which reads
				// instantly as fake. World space means sweeping the beam
				// reveals different dust, the way it should.
				if (DustAmount > 0.0)
				{
					vec3 worldP = (ViewToWorld * vec4(p, 1.0)).xyz;
					worldP.y -= DustTime * DustDrift;   // slow settle
					float d = dustNoise(worldP * DustScale);

					// [FIX] CONTRAST BEFORE THE AVERAGE, or there is nothing
					// left to see.
					//
					// accum is divided by `steps` further down, and averaging
					// a noise field along a ray converges to its MEAN. At the
					// default 24 steps that turned the dust into a flat ~15%
					// dimming of the whole cone with almost no spatial
					// structure -- the motes were being computed correctly
					// and then averaged out of existence.
					//
					// Pushing the field toward its extremes first means the
					// average still carries the difference between a thick
					// patch and a thin one. This does not fight the divide,
					// which the density accumulation needs; it gives the
					// divide something that survives it.
					d = smoothstep(0.22, 0.78, d);

					// Never fully dark: dust thickens the beam in places, it
					// does not punch holes through it.
					contrib *= mix(1.0, d, clamp(DustAmount, 0.0, 1.0));
				}

#if defined(SMOKE_TRANSMITTANCE)
				// [13e] Dimmed by the haze between the eye and this step.
				if (smokeDims)
					contrib *= SmokeTransmittanceTo(smokeKnots, t * smokeScale, smokeShareAtScene, smokeWholeT);
#endif
				accum += contrib;
			}
		}

		t += dt;
	}

	// Normalise by step count so density means the same thing regardless of
	// quality setting -- turning quality down must not turn the beam down.
	//
	// Then multiply by the marched length, which turns the average into an
	// integral along the ray. That is the correct Riemann sum and it is also
	// where the units live: DENSITY IS PER 1000 UNITS, the same convention the
	// fog slab uses, and the 0.001 is what says so.
	//
	// It was missing, and for the whole life of this pass that was invisible.
	// The march used to be clamped against the RAW depth sample -- a 0..1
	// value treated as a distance -- so the length was never more than 1.0 and
	// the scale was accidentally sane. Fixing the depth made the length real,
	// somewhere between a hundred and a couple of thousand units, and the beam
	// came out three orders of magnitude too bright. Straight into an additive
	// pass that runs BEFORE bloom, which then amplified it.
	//
	// Two bugs that had been cancelling each other out, where fixing the first
	// one alone looks like the fix caused the problem.
	accum *= Density / float(steps);
	accum *= (tMax - tMin) * 0.001;

	// ---------------------------------------------------------------------
	// A CONE SEEN END-ON IS A DISC.
	//
	// Look straight down your own torch and the cross-section you are looking
	// through is the whole cone, so it fills the middle of the screen as a
	// soft bright circle. That is not a bug in the integral -- it is what the
	// integral correctly says -- but it is useless: a wash centred on the
	// crosshair carries no information about the beam, because the beam is
	// exactly where you are already looking.
	//
	// And on a flat screen that is most of how you see it. A torch mounted
	// on the view -- or on AttackPos/AttackAngle, which on a flat screen come
	// from the view angles at shooting height (hw_vrmodes.cpp) -- keeps its
	// cone aligned with the camera. What you get is not a beam, it is a
	// permanent bloom-fed halo over the centre of the frame.
	//
	// Note for mounts: in VR AttackPos/AttackAngle are the CONTROLLER, not
	// the view, and the angles are stored offset -- AttackAngle is world yaw
	// minus 90 and AttackPitch is negated (g_game.cpp, hw_vrmodes.cpp). Use
	// yaw = AttackAngle + 90 and Doom pitch = -AttackPitch, or anchor the beam
	// with SetVolumetricBeamAnchor and let the renderer do it.
	//
	// So fade by how well the view axis agrees with the beam axis. In view
	// space the view direction is exactly (0,0,-1), which makes the whole
	// test one component and no extra uniform to pass. Squared, so only the
	// genuinely near-aligned case is touched and a torch held off to one side
	// -- the shot actually worth having -- keeps its full strength.
	//
	// This is a dial rather than a rule because in VR the hands are tracked
	// separately and a hand torch pointed forward is a real thing somebody
	// might want to see.
	//
	// ONLY WHEN THE BEAM'S AXIS ACTUALLY PASSES NEAR THE EYE. (FL-09) The fade
	// used to depend on direction alone, so a VR hand torch pointed where you
	// look kept 15% of its light even though its apex is on a hand a couple
	// of feet away and you see the cone from beside, not end-on. The disc only
	// happens when the axis line runs through (or very near) the eye, so the
	// fade is scaled by that distance: full within a quarter of AxisFadeReach,
	// gone by AxisFadeReach (vol_beam_axisfade_reach, map units). A head or
	// view mount, a few units off, keeps the whole fade; a hand does not.
	// AxisFadeReach 0 restores the old direction-only fade.
	if (AxisFade > 0.0)
	{
		float align = clamp(-BeamDir.z, 0.0, 1.0);
		float nearAxis = 1.0;
		if (AxisFadeReach > 0.0)
		{
			// Eye (the view-space origin) to the beam's axis line.
			vec3 axisToEye = BeamPos - BeamDir * dot(BeamPos, BeamDir);
			nearAxis = 1.0 - smoothstep(AxisFadeReach * 0.25, AxisFadeReach, length(axisToEye));
		}
		accum *= 1.0 - AxisFade * align * align * nearAxis;
	}

	FragColor = vec4(BeamColor * accum, 1.0);
}
