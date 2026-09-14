
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// MULTISAMPLE is defined for the OffsetMS variant (hw_postprocess.h), picked when
// gl_multisample > 1: the scene depth is then a multisampled texture and a plain
// sampler2D reads it as 0. Same split as volumetricbeam.fp and heatmap.fp.
#if defined(MULTISAMPLE)
layout(binding=0) uniform sampler2DMS DepthTexture;
#else
layout(binding=0) uniform sampler2D DepthTexture;
#endif

// ============================================================================
// [HEATREFRACTION] HEAT SHIMMER, PASS 1 OF 2: HOW FAR ONE SOURCE BENDS EACH PIXEL.
// ("Engine docs/FLAME_ENGINE_PLAN.md" F2; the C++ is PPHeatRefraction.)
//
// Drawn once per visible heat source, into a half-resolution RGBA16F texture,
// additively (the first source of an eye draws with no blend, which is the
// clear). Each source adds its own:
//   rg  the screen shift of the image behind it, in scene UV units
//   b   the view depth where the ray enters the heat, times the weight
//   a   the weight
// so heatwarp.fp can recover a weighted mean entry depth b / a.
//
// THE SOURCE is the convex hull of two balls, (SourceStart, RadiusStart) and
// (SourceEnd, RadiusEnd): a ball, a capsule, or a cone that widens as it goes.
// Positions arrive relative to THIS eye, in world axes (GL: y up) and map units,
// and they are per eye (PPHeatRefraction keeps a uniform set for each eye of a
// multiview scene).
//
// WHY WORLD SPACE AND NOT VIEW SPACE. The view matrix carries the pixel stretch
// (1.2 on the vertical axis), so a ball in view space is squashed. The pixel's ray
// is built as the beam builds it (TanHalfFov + ProjOffset) and taken out through
// ViewToWorld with its z at -1, so one step along it is one unit of VIEW DEPTH:
// distances along the unit ray are (view depth x stepLen), and the scene depth
// compares directly.
//
// DEPTH-AWARE. Scene depth nearer than where the ray enters the heat: nothing, so
// a hand or a wall in front stays sharp. Otherwise the hot air the ray crosses is
// the chord from the entry to min(exit, scene depth).
//
// THE BEND is the gradient of world-space value noise at the chord's middle --
// world space, so both eyes see the same moving air at the same depth; screen
// space noise would swim between the eyes -- scaled by how much hot air the ray
// crosses, brought into view axes and turned from an angle into a shift.
// ============================================================================

// volumetricbeam.fp's hash, verbatim, so this noise is the beam dust's noise. A
// copy rather than an #include: GL's post-process compile has no include handling.
float hash13(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

// The gradient of volumetricbeam.fp's valueNoise(p), analytically: the same eight
// lattice hashes and the same smoothstep weights, differentiated. One evaluation
// instead of the four finite differences would take.
vec3 valueNoiseGrad(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	vec3 u = f * f * (3.0 - 2.0 * f);
	vec3 du = 6.0 * f * (1.0 - f);

	float n000 = hash13(i);
	float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
	float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
	float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
	float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
	float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
	float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
	float n111 = hash13(i + vec3(1.0, 1.0, 1.0));

	// valueNoise = n000 + k1 u.x + k2 u.y + k3 u.z + k4 u.x u.y + k5 u.y u.z
	//            + k6 u.z u.x + k7 u.x u.y u.z
	float k1 = n100 - n000;
	float k2 = n010 - n000;
	float k3 = n001 - n000;
	float k4 = n000 - n100 - n010 + n110;
	float k5 = n000 - n010 - n001 + n011;
	float k6 = n000 - n100 - n001 + n101;
	float k7 = -n000 + n100 + n010 - n110 + n001 - n101 - n011 + n111;

	return du * vec3(
		k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
		k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
		k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
}

// A ray from the origin along the unit direction rd, against the ball (c, r).
bool BallInterval(vec3 rd, vec3 c, float r, out float lo, out float hi)
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

// The hull's body: the cone tangent to both balls, between its two circles of
// contact. With s = (ra - rb) / |b - a| the sine of the cone's half-angle and y the
// position along the axis from a, the circles sit at y = ra s and y = |b - a| + rb s,
// and between them the radius is R(y) = (ra - s y) / cos. Inside is
//   cos^2 (|t rd - a|^2 - y^2) - (ra - s y)^2 <= 0,  y = t (rd.u) - (a.u),
// one quadratic in t, clipped to the slab between the circles. The cone's apex is
// always outside that slab, so its mirror half never reaches it, and the result is
// one interval.
bool BodyInterval(vec3 rd, vec3 a, float ra, vec3 b, float rb, out float lo, out float hi)
{
	lo = 0.0;
	hi = -1.0;
	vec3 ab = b - a;
	float len = length(ab);
	float rr = ra - rb;
	if (len <= abs(rr) + 0.001) return false;   // one ball holds the other: the hull is that ball

	vec3 u = ab / len;
	float s = rr / len;
	float c2 = 1.0 - s * s;
	float ya = ra * s;
	float yb = len + rb * s;

	float du = dot(rd, u);
	float au = dot(a, u);
	float da = dot(rd, a);
	float k = ra + s * au;

	float slabLo;
	float slabHi;
	if (abs(du) > 1e-6)
	{
		float t0 = (ya + au) / du;
		float t1 = (yb + au) / du;
		slabLo = min(t0, t1);
		slabHi = max(t0, t1);
	}
	else
	{
		if (-au < ya || -au > yb) return false;
		slabLo = -1e30;
		slabHi = 1e30;
	}

	float qa = c2 - du * du;
	float qb = -2.0 * c2 * (da - du * au) + 2.0 * s * k * du;
	float qc = c2 * (dot(a, a) - au * au) - k * k;

	if (abs(qa) < 1e-6)
	{
		// The ray runs parallel to the cone's surface: the test is linear.
		if (abs(qb) < 1e-6)
		{
			if (qc > 0.0) return false;
			lo = slabLo;
			hi = slabHi;
		}
		else if (qb > 0.0)
		{
			lo = slabLo;
			hi = min(slabHi, -qc / qb);
		}
		else
		{
			lo = max(slabLo, -qc / qb);
			hi = slabHi;
		}
		return hi > lo;
	}

	float disc = qb * qb - 4.0 * qa * qc;
	if (qa < 0.0 && disc < 0.0)
	{
		// Inside the (double) cone all along the ray; the slab keeps the right half.
		lo = slabLo;
		hi = slabHi;
		return hi > lo;
	}
	if (disc < 0.0) return false;

	// Numerically stable roots (volumetricbeam.fp's form): the textbook formula
	// loses the small root at a grazing angle.
	float sq = sqrt(disc);
	float h = -0.5 * (qb + (qb >= 0.0 ? sq : -sq));
	float r0 = h / qa;
	float r1 = (abs(h) > 1e-12) ? qc / h : r0;
	float rLo = min(r0, r1);
	float rHi = max(r0, r1);

	if (qa > 0.0)
	{
		lo = max(slabLo, rLo);
		hi = min(slabHi, rHi);
		return hi > lo;
	}

	// qa < 0: inside is outside the roots, (-inf, rLo] and [rHi, inf), each clipped to
	// the slab. At most one survives; take both ends in case rounding kept a sliver.
	float aLo = slabLo;
	float aHi = min(slabHi, rLo);
	float bLo = max(slabLo, rHi);
	float bHi = slabHi;
	bool hasA = aHi > aLo;
	bool hasB = bHi > bLo;
	if (!hasA && !hasB) return false;
	lo = hasA ? aLo : bLo;
	hi = hasB ? bHi : aHi;
	return true;
}

// The hull is convex and is the union of the two balls and the body, so the ray
// meets it in one interval: the earliest entry and the latest exit of the three.
bool HullInterval(vec3 rd, vec3 a, float ra, vec3 b, float rb, out float lo, out float hi)
{
	lo = 1e30;
	hi = -1e30;
	float pieceLo;
	float pieceHi;
	if (BallInterval(rd, a, ra, pieceLo, pieceHi)) { lo = min(lo, pieceLo); hi = max(hi, pieceHi); }
	if (BallInterval(rd, b, rb, pieceLo, pieceHi)) { lo = min(lo, pieceLo); hi = max(hi, pieceHi); }
	if (BodyInterval(rd, a, ra, b, rb, pieceLo, pieceHi)) { lo = min(lo, pieceLo); hi = max(hi, pieceHi); }
	return hi > lo;
}

void main()
{
	// Nothing: under the additive blend this adds nothing; as the first source of an
	// eye (no blend) it is the clear.
	FragColor = vec4(0.0);

	// This pixel's ray: view space with z at -1, then the same step in world axes.
	vec2 ndc = TexCoord * 2.0 - 1.0;
	vec3 viewRay = vec3((ndc + ProjOffset) * TanHalfFov, -1.0);
	vec3 worldRay = mat3(ViewToWorld) * viewRay;
	float stepLen = length(worldRay);
	if (stepLen < 1e-6) return;
	vec3 rd = worldRay / stepLen;

	// The bounding ball first: most of the screen misses the source, and leaves here
	// for a dot product and a square root.
	vec3 centre = 0.5 * (SourceStart + SourceEnd);
	float reach = 0.5 * length(SourceEnd - SourceStart) + max(RadiusStart, RadiusEnd);
	float boundLo;
	float boundHi;
	if (!BallInterval(rd, centre, reach, boundLo, boundHi) || boundHi <= 0.0) return;

	float tIn;
	float tOut;
	if (!HullInterval(rd, SourceStart, RadiusStart, SourceEnd, RadiusEnd, tIn, tOut) || tOut <= 0.0) return;
	tIn = max(tIn, 0.0);   // the eye inside the heat: the hot air starts at the eye

	// Scene depth, inside the scene viewport (as volumetricbeam.fp reads it).
	vec2 depthUV = SceneOffset + TexCoord * SceneScale;
#if defined(MULTISAMPLE)
	ivec2 depthSize = textureSize(DepthTexture);
	ivec2 depthTexel = clamp(ivec2(depthUV * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
	float rawDepth = texelFetch(DepthTexture, depthTexel, 0).x;
#else
	float rawDepth = texture(DepthTexture, depthUV).x;
#endif
	float linearZ = 1.0 / (clamp(rawDepth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
	float sceneT = linearZ * stepLen;

	if (sceneT <= tIn) return;   // something stands in front of the heat
	float tEnd = min(tOut, sceneT);
	float thickness = tEnd - tIn;
	if (thickness <= 0.0) return;

	// A chord's length rises like a square root from the silhouette, which would draw
	// the source's outline. Weighed by how deep into the source it runs (the chord
	// over the local diameter), it starts from zero smoothly.
	vec3 mid = rd * (0.5 * (tIn + tEnd));
	vec3 axis = SourceEnd - SourceStart;
	float axisLen2 = dot(axis, axis);
	float along = (axisLen2 > 1e-6) ? clamp(dot(mid - SourceStart, axis) / axisLen2, 0.0, 1.0) : 0.0;
	float diameter = 2.0 * max(mix(RadiusStart, RadiusEnd, along), 0.5);
	float body = thickness * clamp(thickness / diameter, 0.0, 1.0);

	// Two octaves of rising noise at the chord's middle, in world space. The second
	// rises faster, so the pattern changes as it climbs rather than sliding up whole.
	vec3 worldMid = ViewToWorld[3].xyz + mid;
	float climb = NoiseTime * NoiseRise * NoiseScale;
	vec3 p1 = worldMid * NoiseScale;
	p1.y -= climb;
	vec3 p2 = worldMid * (NoiseScale * 2.0) + vec3(17.3, 0.0, 41.9);
	p2.y -= climb * 3.2;
	vec3 gradWorld = valueNoiseGrad(p1) + valueNoiseGrad(p2);

	// Into view axes. A gradient goes by the transpose of the matrix that takes view
	// points to world points: row vector times mat3(ViewToWorld).
	vec3 gradView = gradWorld * mat3(ViewToWorld);

	// Bend (radians per map unit of hot air, strength and fade included) times the air
	// crossed gives an angle; an angle over the tangent of the half field of view is
	// NDC, and half of NDC is UV.
	vec2 bendAngle = Bend * body * gradView.xy;
	vec2 shift = 0.5 * bendAngle / TanHalfFov;

	float weight = clamp(body / 8.0, 0.0, 1.0);
	float entryDepth = tIn / stepLen;
	FragColor = vec4(shift, entryDepth * weight, weight);
}
