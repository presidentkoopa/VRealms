
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SharedMarchTexture;	// the shared march, packed (SharedMarchPack below)
layout(binding=1) uniform sampler2D EyeDepthTexture;	// smokedepth.fp for THIS eye, at this resolution: linear view depth

// ============================================================================
// [SHAREDMARCH] E2: ONE MARCH CARRIED INTO AN EYE ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E2; "Engine docs/
// SHARED_MARCH_E2_IMPL_NOTES.md"; the C++ is PPSharedMarchWarp, hw_postprocess.h).
//
// A pass that marches a view ray -- the smoke volume, the emissive volumes -- marches ONCE from a view halfway between the two
// eyes, and this draw carries that result into one eye's texels. The eyes are 6.4 cm apart, about 2 map units; anything more
// than a stride away looks nearly the same to each, and what does not -- the near shell, a volume at the muzzle -- is marched
// per eye by the caller's own fill pass and put over this. Nothing here knows which effect it is serving: it reads one packed
// march and writes the same packed march in this eye's texels. The second caller needed no change to it.
//
// THE PACKED MARCH is SharedMarchPack below: the light the ray gathered, its transmittance, the FRONT and BACK depth of the
// stretch that light came from, the stretch's representative depth, and a change level the caller may use. Front and back are
// what make the carry possible: this eye's own scene may cut the stretch shorter than the shared view's did, and with the two
// ends known the optical depth can be taken as the share of the stretch the eye can still see.
//
// HOW A TEXEL IS FOUND. This texel's ray leaves the eye; the shared view saw the same smoke from an eye-half aside, so the
// texel holding it is not this texel. Starting from the direction alone (exact for a ray that never ends), the shared texel's
// own depth says where its light is; that point is projected back into this eye, and the miss moves the guess. Three turns:
// the disparity at the near split is about a degree and the depth field is smooth over a texel, so it settles.
//
// WHERE IT CANNOT BE FOUND -- the ray leaves the shared view, the turns do not settle, the neighbourhood straddles a depth
// edge, or this eye's scene reaches past where the shared stretch ended -- the texel is written as a HOLE
// (SHARED_MARCH_HOLE in the depth field) and the caller's fill pass marches it per eye at a low step count. Marking too many
// holes costs steps and never costs accuracy, so every test errs toward a hole.
//
// NETPLAY: presentation only. Nothing here is simulated and no two machines need agree on it.
// ============================================================================

const int SHARED_WARP_TURNS = 3;			// how many times the guess is corrected
const float SHARED_WARP_SETTLED = 1.5;		// texels of miss a settled guess may still have

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

// A view ray for a texel of a view with these projection terms: view space with z at -1, so a point at view depth d is this
// times d. The same ray smokemarch.fp and emissivevolume.fp build.
vec3 SharedMarchRay(vec2 coord, vec2 tanHalfFov, vec2 projOffset)
{
	return vec3((coord * 2.0 - 1.0 + projOffset) * tanHalfFov, -1.0);
}

// Where a view-space point lands on that view's screen, in TexCoord. z is 0 behind the view's plane, where there is no answer.
vec3 SharedMarchProject(vec3 viewPoint, vec2 tanHalfFov, vec2 projOffset)
{
	float depth = -viewPoint.z;
	if (!(depth > 1e-3))
		return vec3(0.0, 0.0, 0.0);
	vec2 ndc = (viewPoint.xy / depth) / max(tanHalfFov, vec2(1e-6)) - projOffset;
	return vec3(ndc * 0.5 + 0.5, 1.0);
}

void main()
{
	ivec2 texel = ivec2(gl_FragCoord.xy);
	ivec2 size = max(textureSize(SharedMarchTexture, 0), ivec2(1));
	float sceneZ = texelFetch(EyeDepthTexture, texel, 0).r;

	// [SHAREDMARCH] E2: A HOLE WHEREVER THIS EYE'S OWN SCENE HAS AN EDGE within a couple of texels. That band is where the
	// shared view, a head's half aside, was looking at something else -- and a carried value there is smeared across a
	// silhouette. Asking it of THIS eye's depth needs no guess about where the same edge sits in the other view, which is the
	// whole difficulty: the shared view's own neighbourhood test (further down) cannot see an edge that has moved.
	//
	// The SECOND difference, and only at two texels out. Second, so a steeply receding floor -- a straight ramp in depth --
	// reads as no edge at all. Two out, because smokedepth.fp lays the near and the far of each 2x2 on a checkerboard, so
	// neighbouring texels disagree by design and only texels of the same colour may be compared.
	float edgeTolerance = max(DepthToleranceUnits, DepthTolerance * sceneZ);
	for (int axis = 0; axis < 2; axis++)
	{
		ivec2 step2 = axis == 0 ? ivec2(2, 0) : ivec2(0, 2);
		float lo = texelFetch(EyeDepthTexture, clamp(texel - step2, ivec2(0), size - ivec2(1)), 0).r;
		float hi = texelFetch(EyeDepthTexture, clamp(texel + step2, ivec2(0), size - ivec2(1)), 0).r;
		if (abs(lo + hi - 2.0 * sceneZ) > edgeTolerance)
		{
			FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, SHARED_MARCH_HOLE, 0.0);
			return;
		}
	}

	// This eye's ray, and the same direction as the shared view sees it: a ray that never ends lands there exactly, which is
	// the right first guess for the far light this pass exists for.
	vec3 eyeRay = SharedMarchRay(TexCoord, TanHalfFov, ProjOffset);
	vec3 sharedDirection = mat3(EyeToShared) * eyeRay;
	vec3 guess = SharedMarchProject(sharedDirection, SharedTanHalfFov, SharedProjOffset);
	if (guess.z <= 0.0)
	{
		FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, SHARED_MARCH_HOLE, 0.0);
		return;
	}

	// Each turn: the depth the shared texel says its light is at, that point carried into this eye, and the miss on this eye's
	// screen moved back onto the shared one. The two screens are the same size and nearly the same shape, so the move is the
	// ratio of their ray terms.
	vec2 coord = guess.xy;
	vec2 gain = TanHalfFov / max(SharedTanHalfFov, vec2(1e-6));
	vec2 texelStep = 1.0 / vec2(size);
	bool settled = false;
	for (int turn = 0; turn < SHARED_WARP_TURNS; turn++)
	{
		if (any(lessThan(coord, vec2(-0.02))) || any(greaterThan(coord, vec2(1.02))))
			break;
		ivec2 at = clamp(ivec2(coord * vec2(size)), ivec2(0), size - ivec2(1));
		SharedMarchSample probe = SharedMarchUnpack(texelFetch(SharedMarchTexture, at, 0));
		// Where the light on that ray is. A texel the shared view found nothing on carries no depth of its own, so this eye's
		// own scene depth stands in: the guess then walks toward the texel that looks at this eye's surface.
		float probeDepth = probe.Depth > 0.0 ? probe.Depth : max(sceneZ, 1.0);
		vec3 sharedPoint = SharedMarchRay(coord, SharedTanHalfFov, SharedProjOffset) * probeDepth;
		vec3 eyePoint = (SharedToEye * vec4(sharedPoint, 1.0)).xyz;
		vec3 backOnScreen = SharedMarchProject(eyePoint, TanHalfFov, ProjOffset);
		if (backOnScreen.z <= 0.0)
			break;
		// The move is made whether or not this turn is the last: a guess that already lands within a texel and a half is close
		// enough to stop turning, but its own correction is still the best one there is and it costs nothing to take.
		vec2 miss = TexCoord - backOnScreen.xy;
		bool close = all(lessThan(abs(miss), texelStep * SHARED_WARP_SETTLED));
		coord += miss * gain;
		if (close)
		{
			settled = true;
			break;
		}
	}

	if (!settled || any(lessThan(coord, vec2(0.0))) || any(greaterThan(coord, vec2(1.0))))
	{
		FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, SHARED_MARCH_HOLE, 0.0);
		return;
	}

	// The four shared texels around the settled guess, each weighed by how far the guess is from it and by whether it agrees
	// about depth with the nearest one. A neighbourhood that straddles a depth edge -- which is exactly where one eye sees what
	// the other cannot -- disagrees, and the texel becomes a hole rather than a smear across the edge.
	vec2 at = coord * vec2(size) - 0.5;
	ivec2 base = ivec2(floor(at));
	vec2 frac = at - vec2(base);
	SharedMarchSample taps[4];
	float bilinear[4];
	for (int i = 0; i < 4; i++)
	{
		ivec2 offset = ivec2(i & 1, i >> 1);
		ivec2 q = clamp(base + offset, ivec2(0), size - ivec2(1));
		taps[i] = SharedMarchUnpack(texelFetch(SharedMarchTexture, q, 0));
		vec2 share = mix(vec2(1.0) - frac, frac, vec2(offset));
		bilinear[i] = share.x * share.y;
	}

	// What the neighbourhood agrees on, and how much it does not. TWO disagreements matter, and they are not the same thing:
	//   - WHERE THE RAYS WERE STOPPED. That is geometry, and a jump in it is exactly the edge one eye can see behind and the
	//     other cannot. Its tolerance is a share of the distance, as a surface's would be. Only a ray that WAS stopped has a
	//     stop (back's sign): four rays that simply ran out of light have no geometry between them to disagree about, and
	//     comparing where their light happened to peter out calls a soft cloud edge a wall. Rays of both kinds in one
	//     neighbourhood is itself the edge.
	//   - WHERE THE LIGHT SAT (the representative depth). Inside a thick cloud that wanders a few units from texel to texel and
	//     means nothing at all -- it is a weighted mean through a volume, not a surface -- so its tolerance is taken against the
	//     STRETCH'S OWN LENGTH. Half the stretch is a real move; six units in a cloud six hundred deep is not.
	// Judging either by a surface's tolerance was the first version of this pass, and the mirror caught it calling nine texels
	// in ten an edge: a correct image at no saving at all.
	float centreDepth = 0.0;
	float bestWeight = -1.0;
	float lowDepth = 1e30;
	float highDepth = -1e30;
	float lowStop = 1e30;
	float highStop = -1e30;
	float lowFront = 1e30;
	float longestReach = 0.0;
	int litTaps = 0;
	int stoppedTaps = 0;
	for (int i = 0; i < 4; i++)
	{
		if (taps[i].Depth > 0.0)
		{
			litTaps++;
			lowDepth = min(lowDepth, taps[i].Depth);
			highDepth = max(highDepth, taps[i].Depth);
			lowFront = min(lowFront, taps[i].Front);
			longestReach = max(longestReach, abs(taps[i].Back));
			if (taps[i].Back > 0.0)
			{
				stoppedTaps++;
				lowStop = min(lowStop, taps[i].Back);
				highStop = max(highStop, taps[i].Back);
			}
			if (bilinear[i] > bestWeight)
			{
				bestWeight = bilinear[i];
				centreDepth = taps[i].Depth;
			}
		}
	}
	if (litTaps == 0)
	{
		// Nothing in the neighbourhood: this eye sees nothing here either. Clear, not a hole -- the composite discards it.
		FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, 0.0, 0.0);
		return;
	}
	float tolerance = max(DepthToleranceUnits, DepthTolerance * centreDepth);
	float stretchTolerance = max(tolerance, 0.5 * max(longestReach - lowFront, 0.0));
	bool mixedStops = stoppedTaps > 0 && stoppedTaps < litTaps;
	bool stopEdge = stoppedTaps >= 2 && highStop - lowStop > tolerance;
	if (litTaps < 4 || mixedStops || stopEdge || highDepth - lowDepth > stretchTolerance)
	{
		FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, SHARED_MARCH_HOLE, 0.0);
		return;
	}

	vec3 light = vec3(0.0);
	float transmittance = 0.0;
	float front = 0.0;
	float back = 0.0;
	float depth = 0.0;
	float change = 0.0;
	float total = 0.0;
	bool stopped = false;		// any tap whose ray was STOPPED where its back says (back's sign): the disocclusion test below
	for (int i = 0; i < 4; i++)
	{
		float weight = bilinear[i];
		if (!(weight > 0.0))
			continue;
		light += taps[i].Light * weight;
		transmittance += taps[i].Transmittance * weight;
		front += taps[i].Front * weight;
		back += abs(taps[i].Back) * weight;		// only the magnitude is the depth
		stopped = stopped || taps[i].Back > 0.0;	// err toward a hole: one stopped tap in the four is enough
		depth += taps[i].Depth * weight;
		change = max(change, taps[i].Change);
		total += weight;
	}
	if (!(total > 1e-5))
	{
		FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, SHARED_MARCH_HOLE, 0.0);
		return;
	}
	light /= total;
	transmittance /= total;
	front /= total;
	back /= total;
	depth /= total;

	// The stretch carried into this eye. The two views differ by a step across the head, which is square to the way both look,
	// so a world point keeps its view depth and the two ends need no carrying of their own; what does change is where THIS
	// eye's scene stops the ray.
	//   - the scene is in front of the whole stretch: this eye sees none of it. Clear.
	//   - the shared ray was STOPPED at its back (back's sign) and this eye's scene reaches past that stop: this eye is looking
	//     around an edge at light the shared view never marched. A hole. A stretch that merely ran out is not: there is nothing
	//     past it to miss.
	//   - otherwise the share of the stretch this eye can still see carries the optical depth: T to the power of the share,
	//     and the light with it. A stretch that only glows (transmittance 1) takes the share straight, which is what a line
	//     integral of emission does.
	float span = max(back - front, 1e-4);
	if (sceneZ <= front)
	{
		FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, 0.0, 0.0);
		return;
	}
	if (stopped && sceneZ > back + tolerance)
	{
		FragColor = SharedMarchPack(vec3(0.0), 1.0, 0.0, 0.0, SHARED_MARCH_HOLE, 0.0);
		return;
	}
	float share = clamp((min(sceneZ, back) - front) / span, 0.0, 1.0);
	float eyeTransmittance = pow(clamp(transmittance, 0.0, 1.0), share);
	float opacity = 1.0 - clamp(transmittance, 0.0, 1.0);
	vec3 eyeLight = opacity > 1e-5 ? light * ((1.0 - eyeTransmittance) / opacity) : light * share;

	FragColor = SharedMarchPack(eyeLight, eyeTransmittance, front, min(back, sceneZ), min(depth, max(sceneZ, front)), change);
}
