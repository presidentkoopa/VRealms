
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SmokeDepthTexture;	// smokedepth.fp: the linear view depth to march to, this pass's resolution
layout(binding=1) uniform sampler3D DensityLatest;		// the volume: density r, heat g, latest simulation state (linear)
layout(binding=2) uniform sampler3D DensityPrevious;	// the same one simulation step earlier (linear)
layout(binding=3) uniform sampler3D TileActive;			// one texel per tile: 1 = the tile may hold smoke

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
// LightColor, dimmed by the smoke already in front of it. In 13c LightColor is one value for the frame
// (the smoke's tint x its ambient x the light of the sector the view is in). 13d's light grid replaces
// it with light that varies through the volume.
// ============================================================================

float InterleavedGradientNoise(vec2 pixel)
{
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
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

	float transmittance = 1.0;
	vec3 light = vec3(0.0);
	for (int i = 0; i < count; i++)
	{
		float t = t0 + (float(i) + jitter) * dt;
		float density = DensityAt(CellAt(rd * t));
		if (density <= 0.0)
			continue;
		float stepTransmittance = exp(-density * Extinction * dt);
		light += transmittance * (1.0 - stepTransmittance) * LightColor;
		transmittance *= stepTransmittance;
		if (transmittance < 1.0 / 256.0)
			break;
	}
	FragColor = vec4(light, transmittance);
}
