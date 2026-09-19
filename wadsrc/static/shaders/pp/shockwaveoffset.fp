
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// MULTISAMPLE is defined for the ShockwaveShaderMS variant (hw_postprocess.h), picked when gl_multisample > 1: the scene depth
// is then a multisampled texture and a plain sampler2D reads it as 0. Same split as heatoffset.fp.
#if defined(MULTISAMPLE)
layout(binding=0) uniform sampler2DMS DepthTexture;
#else
layout(binding=0) uniform sampler2D DepthTexture;
#endif

// ============================================================================
// [SHOCKWAVE] BLAST RIPPLES: HOW FAR ONE RIPPLE BENDS EACH PIXEL.
// ("Engine docs/BLAST_RIPPLE_PLAN.md" 3d; the C++ is PPHeatRefraction, the maths hw_shockwavecore.h.)
//
// A second kind of source for the heat shimmer pass. It writes the heat offset texture's encoding, additively after the
// heat sources, and heatwarp.fp bends the image by the sum:
//   rg  the screen shift of the image behind it, in scene UV units
//   b   the view depth where the ray enters the ripple, times the weight
//   a   the weight
//
// THE RIPPLE is a spherically symmetric shell around Centre -- relative to THIS eye, in GL world axes and map units, per eye
// like a heat source. Along the radius rho its air is denser than still air by the profile g(u), u = (rho - CrestRadius) /
// HalfThickness: a crest bump (1 - u^2)^2 for |u| < 1, minus Trough times the same bump centred at u = -2, the thin air behind
// the crest. There is nothing outside rho in [crest - 3h, crest + h].
//
// THE BEND. Light turns toward denser air by the profile's gradient across the ray. For a spherical shell the gradient is
// radial, and its part across a ray always lies along cPerp, the perpendicular from the ray to the centre. So a ray's whole
// deflection is
//   deflection = -cPerp x J x Bend,   J = the integral along the ray of g'(u) / (h rho) dt,
// a world-space angle. There is no noise, so both eyes see the same ring at the same depth, and no ray marching, so a thin
// shell is never stepped over.
//
// THE INTEGRAL. With x = t - (the ray's closest approach) and b the ray's closest distance to the centre, rho = sqrt(b^2 + x^2).
// The profile's four lobes in rho are split at crest + h, crest, crest - h, crest - 2h and crest - 3h, and the ray meets each
// lobe over at most two x intervals (the near side and the far side). Inside one interval the integrand is smooth, and 3-point
// Gauss-Legendre is within 2.5% of the exact integral over every ray of the float64 mirror (ripple/mirror_ripple.py, "SI").
// Each interval is clipped to the ray between the eye and the scene depth: a ripple behind a wall bends nothing, and the near
// half of a shell in front of a wall still bends the wall.
//
// INTO THE SCREEN. The deflected direction goes back into this eye's view vector through the inverse of ViewToWorld's 3x3
// (which carries the pixel stretch) and is projected as the pixel's own ray, so the shift is the screen movement of what the
// ray sees, in scene UV.
//
// ONLY OVER ITS RECTANGLE. PPHeatRefraction draws each ripple over its conservative screen rectangle (hw_shockwavecore.h,
// ProjectBall) and hands this draw's place in the offset texture as RectScale and RectOffset, so TexCoord (0..1 across the
// draw's viewport) becomes the texel's scene UV. A draw that is an eye's first covers the whole viewport (RectScale 1,
// RectOffset 0): with no blend, its zeros are the clear.
// ============================================================================

// A ray from the eye along the unit direction rd, against the ball (c, r).
bool ShockBallInterval(vec3 rd, vec3 c, float r, out float lo, out float hi)
{
	float along = dot(rd, c);
	float disc = along * along - dot(c, c) + r * r;
	if (disc < 0.0)
	{
		lo = 0.0;
		hi = -1.0;
		return false;
	}
	float q = sqrt(disc);
	lo = along - q;
	hi = along + q;
	return true;
}

// The profile's slope g'(u): the crest bump's, minus Trough times the thin-air bump's.
float ShockProfileSlope(float u)
{
	float slope = 0.0;
	if (abs(u) < 1.0) slope = -4.0 * u * (1.0 - u * u);
	float v = u + 2.0;
	if (abs(v) < 1.0) slope += Trough * 4.0 * v * (1.0 - v * v);
	return slope;
}

// The integrand g'(u) / (h rho) at x.
float ShockIntegrand(float x, float b2)
{
	float rho = sqrt(b2 + x * x);
	return ShockProfileSlope((rho - CrestRadius) / HalfThickness) / (HalfThickness * max(rho, 0.001));
}

// 3-point Gauss-Legendre over [x0, x1].
float ShockIntegrate(float x0, float x1, float b2)
{
	float mid = 0.5 * (x0 + x1);
	float halfWidth = 0.5 * (x1 - x0);
	float offset = 0.7745966692 * halfWidth;
	return halfWidth * (0.5555555556 * (ShockIntegrand(mid - offset, b2) + ShockIntegrand(mid + offset, b2)) +
		0.8888888889 * ShockIntegrand(mid, b2));
}

void main()
{
	// Nothing: under the additive blend this adds nothing; as an eye's first draw (no blend) it is the clear.
	FragColor = vec4(0.0);

	// This texel's scene UV, and its ray: view space with z at -1, then the same step in world axes (heatoffset.fp's ray).
	vec2 sceneUV = RectOffset + TexCoord * RectScale;
	vec2 ndc = sceneUV * 2.0 - 1.0;
	vec3 viewRay = vec3((ndc + ProjOffset) * TanHalfFov, -1.0);
	mat3 viewToWorld = mat3(ViewToWorld);
	vec3 worldRay = viewToWorld * viewRay;
	float stepLen = length(worldRay);
	if (stepLen < 1e-6 || HalfThickness <= 0.0) return;
	vec3 rd = worldRay / stepLen;

	// The shell's bounding ball first: most texels, even inside the rectangle, miss it.
	float outer = CrestRadius + HalfThickness;
	float ballLo;
	float ballHi;
	if (!ShockBallInterval(rd, Centre, outer, ballLo, ballHi) || ballHi <= 0.0) return;

	// Scene depth, inside the scene viewport (as heatoffset.fp reads it).
	vec2 depthUV = SceneOffset + sceneUV * SceneScale;
#if defined(MULTISAMPLE)
	ivec2 depthSize = textureSize(DepthTexture);
	ivec2 depthTexel = clamp(ivec2(depthUV * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
	float rawDepth = texelFetch(DepthTexture, depthTexel, 0).x;
#else
	float rawDepth = texture(DepthTexture, depthUV).x;
#endif
	float linearZ = 1.0 / (clamp(rawDepth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
	float sceneT = linearZ * stepLen;
	if (sceneT <= max(ballLo, 0.0)) return;   // something stands in front of the whole ripple

	// The ray's closest approach to the centre, and the perpendicular from the ray to the centre.
	float closest = dot(rd, Centre);
	vec3 cPerp = Centre - closest * rd;
	float b2 = dot(cPerp, cPerp);
	float b = sqrt(b2);

	// The ray from the eye (t = 0) to the scene depth, as x = t - closest.
	float xNear = -closest;
	float xFar = sceneT - closest;

	float integral = 0.0;
	float inside = 0.0;     // how much of the clipped ray lies in the shell
	float entry = 1e30;     // where it first enters, as t
	for (int lobe = 0; lobe < 4; lobe++)
	{
		float rhoOut = CrestRadius + float(1 - lobe) * HalfThickness;
		float rhoIn = max(CrestRadius - float(lobe) * HalfThickness, 0.0);
		if (rhoOut <= b) continue;
		// (rho - b)(rho + b) rather than rho^2 - b^2: a grazing ray on a big ripple keeps its precision.
		float xOut = sqrt(max((rhoOut - b) * (rhoOut + b), 0.0));
		float xIn = sqrt(max((rhoIn - b) * (rhoIn + b), 0.0));
		for (int nearSide = 0; nearSide < 2; nearSide++)
		{
			float x0 = (nearSide == 1) ? -xOut : xIn;
			float x1 = (nearSide == 1) ? -xIn : xOut;
			x0 = max(x0, xNear);
			x1 = min(x1, xFar);
			if (x1 <= x0) continue;
			integral += ShockIntegrate(x0, x1, b2);
			inside += x1 - x0;
			entry = min(entry, x0 + closest);
		}
	}
	if (inside <= 0.0) return;

	// The deflection, a world-space angle, back into this eye's view vector; projected as the pixel's ray (z at -1).
	vec3 deflection = -cPerp * (integral * Bend);
	vec3 w = inverse(viewToWorld) * (deflection * stepLen);
	vec2 bent = (viewRay.xy + w.xy) / max(1.0 - w.z, 0.5);
	vec2 shift = 0.5 * (bent - viewRay.xy) / TanHalfFov;

	// Weighed by how much of the ray lies in the shell over its thickness, so the silhouette starts smoothly (the heat rule).
	float weight = clamp(inside / (2.0 * HalfThickness), 0.0, 1.0);
	float entryDepth = max(entry, 0.0) / stepLen;
	FragColor = vec4(shift, entryDepth * weight, weight);
}
