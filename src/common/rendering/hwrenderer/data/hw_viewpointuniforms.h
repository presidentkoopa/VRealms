/*
** hw_viewpointuniforms.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "matrix.h"

struct HWDrawInfo;

enum class ELightBlendMode : uint8_t
{
	CLAMP = 0,
	CLAMP_COLOR = 1,
	NOCLAMP = 2,

	DEFAULT = CLAMP,
};

struct HWViewpointUniforms
{
	VSMatrix mProjectionMatrix;
	VSMatrix mViewMatrix;
	VSMatrix mNormalViewMatrix;
	FVector4 mCameraPos;
	FVector4 mClipLine;

	float mGlobVis = 1.f;
	int mPalLightLevels = 0;
	int mViewHeight = 0;
	float mClipHeight = 0.f;
	float mClipHeightDirection = 0.f;
	int mShadowmapFilter = 1;

	int mLightBlendMode = 0;
	int mPadding0 = 0;

	// [BB] Glow wave. Scene-global -- one wave for the frame, four channels
	// reading it at their own phase -- so it lives here rather than in
	// StreamData. StreamData is the per-draw block and its size divides
	// 64KB into MAX_STREAM_DATA draws; spending 64 bytes of it on values
	// that are identical in every draw would cost batching in every frame of
	// the game forever, to say the same thing thirty-four times.
	//
	// xyzw packing, and it has to match the ViewpointData struct in
	// vk_shader.cpp and the ViewpointUBO block in gl_shader.cpp exactly:
	//
	// THERE ARE FOUR LISTS, NOT THREE. Vulkan reaches this block through
	// `viewpoints[HW_VIEWPOINT_INDEX]`, so vk_shader.cpp also carries a
	// `#define` per field mapping the bare name onto that array access. A
	// field can be present and correctly aligned in all three declarations and
	// still fail to compile on Vulkan with "undeclared identifier", while
	// OpenGL -- where the block is declared plainly and no defines exist --
	// runs perfectly. That is how uTornadoCol shipped broken for a session.
	//
	// Two checks, both one line, and the second is the one people forget:
	//
	//   the three declarations agree, in order:
	//     diff <(grep -oP '(?<=FVector4 )m[A-Za-z0-9]+' hw_viewpointuniforms.h) \
	//          <(sed -n '/vec4 uGlowWave;/,/};/p' gl_shader.cpp | grep -oP '(?<=vec4 )u[A-Za-z0-9]+')
	//
	//   every VK field is also #defined:
	//     comm -23 <(...VK struct fields, sorted...) \
	//              <(grep -oP '(?<=#define )u[A-Za-z0-9]+' vk_shader.cpp | sort -u)
	//   Anything this prints will not compile under Vulkan.
	//
	//   mGlowWave        wavelength, speed, sharpness, shape
	//   mGlowWaveDepth   reach swing, brightness swing, colour slide, detune
	//   mGlowWavePhase   wall top, wall bottom, flat floor, flat ceiling
	//   mGlowWaveOrigin  x, y, z, per-room seed scatter
	//
	// Wavelength 0 means the whole feature is off and every glow block falls
	// through to exactly the arithmetic it did before.
	FVector4 mGlowWave = { 0.f, 0.f, 1.f, 1.f };
	FVector4 mGlowWaveDepth = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mGlowWavePhase = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mGlowWaveOrigin = { 0.f, 0.f, 0.f, 0.f };

	// [BB] Darkness as a shader term. Same packing rules as the wave above,
	// and it has to match both GLSL declarations exactly.
	//
	//   mDarkness       mode, adjust, min light, pre-gain
	//   mDarkness2      post-gain, distance depth, distance range, unused
	//   mDarkness3      height depth, height reference, height range, unused
	//
	// Mode 0 means off and every fragment keeps the light it already had.
	FVector4 mDarkness = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mDarkness2 = { 0.f, 0.f, 2048.f, 0.f };
	FVector4 mDarkness3 = { 0.f, 0.f, 256.f, 0.f };

	// [BB] Fog slab -- fog with a top. Same packing discipline as above.
	//
	//   mFogSlab       top Z, density, soft edge, scatter
	//   mFogSlabColor  r, g, b, wake strength
	//   mFogSlabWake   x, y, z, wake radius
	//
	// Density 0 means off and the fragment shader falls straight through.
	FVector4 mFogSlab = { 0.f, 0.f, 24.f, 0.f };
	FVector4 mFogSlabColor = { 1.f, 0.19f, 0.09f, 0.f };
	FVector4 mFogSlabWake = { 0.f, 0.f, 0.f, 0.f };

	// [BB] The flashlight cone, in WORLD space, so the fog slab can be lit by
	// it. The volumetric beam itself is a postprocess pass working in VIEW
	// space, and its uniforms are not reachable from main.fp -- so rather than
	// plumb a second copy of the beam through the postprocess chain, the three
	// numbers the scatter term needs are handed to the fragment shader here.
	//
	//   mFogBeamPos  xyz world position, w beam length (0 = no beam)
	//   mFogBeamDir  xyz world direction, w cos(inner angle)
	//   mFogBeamCol  rgb colour, w cos(outer angle)
	FVector4 mFogBeamPos = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mFogBeamDir = { 0.f, 0.f, 1.f, 1.f };
	FVector4 mFogBeamCol = { 1.f, 1.f, 1.f, 1.f };

	// [BB] mFogSlabExtra: x wake strength, y glow pickup, z torch-beam falloff
	// for the fog glow (written with mFogBeam* in hw_drawinfo.cpp; 0 = linear),
	// w spare.
	//
	// POSITION IS LOAD-BEARING. This sits between mFogBeamCol and mSweepFill
	// because that is where both GLSL copies put it, and a uniform block is
	// matched by OFFSET, not by name. It was declared at the end of this
	// struct once: every field after uFogBeamCol then read the one before it,
	// so beam count came from mBeamFX, resolved as zero, and no beam ever drew
	// -- silently, with no error anywhere, because a mismatched block is still
	// a valid block.
	FVector4 mFogSlabExtra = { 0.f, 0.f, 0.f, 0.f };

	// [BB] Sweep fill -- the pattern drawn INSIDE a band.
	//
	//   mSweepFill    spacing U, spacing V, line width, line softness
	//   mSweepFill2   rotation (deg), drift, major every N, jitter
	//   mSweepFill3   gradient amount, gradient axis, flicker, major boost
	//   mSweepFillCol rgb line colour, w gap fill amount
	//
	// The band's OWN colour is the field; this colour is the lines. Gap 0
	// means only the lines are lit and the room shows between them, which is
	// what reads as lasers rather than as a lit pane.
	//
	// Shared rather than per band, deliberately -- per band would mean another
	// vec4[8] in StreamData and a permanent draw-batching cost. Only the fill
	// MODE is per band, packed into the draw mode's spare bits.
	FVector4 mSweepFill = { 64.f, 64.f, 3.f, 1.5f };
	FVector4 mSweepFill2 = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mSweepFill3 = { 0.f, 0.f, 0.f, 2.f };
	FVector4 mSweepFillCol = { 1.f, 1.f, 1.f, 0.f };

	// [BB] Beams -- real ones. A segment lit per pixel by distance, so it is
	// continuous at any length, wraps every surface, and lights what it passes.
	//
	//   mBeamA[i]    xyz start, w core thickness
	//   mBeamB[i]    xyz end,   w softness (how far the halo reaches)
	//   mBeamCol[i]  rgb colour, w intensity
	//   mBeamParams  x count, y halo strength, z fog scatter, w air glow --
	//                [BEAMLINES] now the count of COMPACTED uploads and the
	//                LARGEST uploaded air glow; per-line look is mBeamLook
	//
	// In the viewpoint block rather than StreamData: these are scene-global,
	// and StreamData's size divides 64KB into MAX_STREAM_DATA draws.
	FVector4 mBeamA[128];
	FVector4 mBeamB[128];
	FVector4 mBeamCol[128];
	FVector4 mBeamParams = { 0.f, 0.35f, 1.f, 0.f };

	// [BB] What happens ALONG the beam, which is what stops it being a stick.
	//
	//   x  scroll speed   energy travelling from muzzle to impact
	//   y  scroll depth   how much it modulates; 0 = a smooth beam
	//   z  taper          how much thinner at the muzzle end than the impact
	//   w  impact flare   brightness boost at the far end, where it lands
	//
	// All four ride the position ALONG the segment, which the closest-approach
	// solve already produces -- so none of them costs a second pass over the
	// beam, only arithmetic on a number that was already in hand.
	FVector4 mBeamFX = { 0.f, 0.f, 0.f, 0.f };

	// [BB] The fog slab's SURFACE, animated.
	//
	//   x amplitude (world units the top rises and falls)
	//   y wavelength
	//   z speed
	//   w cross-swell ratio -- a second wave at an angle to the first, so the
	//     surface rolls rather than corrugating in one direction
	//
	// Amplitude 0 leaves the top perfectly flat, exactly as before.
	//
	// Appended LAST here and last in both GLSL copies, in one change. See
	// mFogSlabExtra above for what happens when those two facts stop being
	// true.
	FVector4 mFogSurf = { 0.f, 256.f, 1.f, 0.6f };

	// [BB] mSweepAir: x how strongly the band's lattice is drawn IN THE AIR,
	// as opposed to on the surfaces it lands on. 0 is the old behaviour.
	// Everything else about it -- colour, density, width, softness, rotation,
	// drift, flicker, jitter, major lines -- comes from the same uniforms the
	// painted version uses, so one page drives both and they cannot drift.
	FVector4 mSweepAir = { 0.f, 0.f, 0.f, 0.f };

	// [BB] mFogSlab2: x the layer's BOTTOM, yzw spare.
	//
	// Default is far below any map, which makes the second smoothstep 1 and
	// leaves the old half-space behaviour untouched. Raise it and the slab
	// becomes a layer: ceiling fog, a floating band, or a drain by walking it
	// up toward the top.
	FVector4 mFogSlab2 = { -32768.f, 0.f, 0.f, 0.f };

	// [BB] A tornado. Density near a vertical axis instead of below a plane --
	// the same fog, shaped into a funnel you can stand inside.
	//
	//   mTornado   x world X, y world Z, z base height, w top height
	//   mTornado2  x base radius, y top radius, z density, w swirl depth
	//   mTornado3  x spin, y twist, z lean, w lean period
	//
	// Density 0 is off, and it is the FIRST thing tested -- this is the most
	// expensive effect in the file, because unlike a knee-high layer it does
	// not early out for most of the screen when you are looking at one.
	FVector4 mTornado = { 0.f, 0.f, 0.f, 512.f };
	FVector4 mTornado2 = { 48.f, 320.f, 0.f, 0.5f };
	FVector4 mTornado3 = { 2.f, 8.f, 0.f, 6.f };

	// The funnel's OWN colour, and its own torch response in .w. Without
	// this it could only ever be a tint of the fog layer, and its scatter
	// came from a dial that is zero whenever floor fog is switched off --
	// which is exactly when a tornado standing in clear air needs it.
	FVector4 mTornadoCol = { 0.55f, 0.6f, 0.7f, 1.2f };

	// [BB] DISTURBANCES -- one primitive, five effects.
	//
	// A wake, a ripple, an ignition, fog draining from a point and a monster
	// shouldering mist aside are the same function: a point, a radius, an age,
	// a strength and a sign. They differ only in whether the radius grows with
	// age, and whether the result subtracts density, adds it, or adds light.
	//
	// So there is one array rather than five features, and everything built on
	// it after this is a ZScript call with no engine change at all.
	//
	//   mFogDisturbA[i]  xyz world point (shader space), w radius
	//   mFogDisturbB[i]  x age in seconds, y strength, z speed, w mode
	//
	//   mode 0 DISC      fixed radius, thins. The wake, and displacers.
	//   mode 1 RIPPLE    a ring at r = age*speed, oscillating, decaying
	//   mode 2 IGNITE    expanding sphere that adds LIGHT, not density
	//   mode 3 GOUT      expanding disc that ADDS density -- a vent
	FVector4 mFogDisturbA[32];
	FVector4 mFogDisturbB[32];

	// DENSITY IS NOT ONE NUMBER ANY MORE. Real mist pools: thick in corners,
	// thin in the open. A noise field over the horizontal plane scaling the
	// density is the single biggest thing separating this from a filter, and
	// drifting it slowly makes the banks move through a room on their own.
	//   x cell scale, y depth 0..1, z drift X, w drift Y
	FVector4 mFogNoise = { 0.004f, 0.f, 0.f, 0.f };

	// TENDRILS, as a lattice rather than as objects -- the tornado's maths at
	// small scale, one per cell of a fract() grid, so four hundred of them
	// cost what one costs. Same trick as the sweep's laser lattice.
	//   mFogTendril   x cell spacing, y radius, z height, w density
	//   mFogTendril2  x rise speed, y phase spread, z lean, w taper
	FVector4 mFogTendril = { 96.f, 10.f, 96.f, 0.f };
	FVector4 mFogTendril2 = { 0.6f, 1.f, 6.f, 1.6f };

	// The wake, stretched along the direction of travel. A disc is a hole you
	// carry; an ellipse is a corridor you carve.
	//   x velocity X, y velocity Y (shader space), z stretch,
	//   w fog IGNITE colour packed 1 + 0xRRGGBB, 0 = unset (SetFogIgniteColor)
	FVector4 mFogWake2 = { 0.f, 0.f, 0.f, 0.f };

	// A sweep band pushes mist ahead of it and leaves it thin behind.
	//   x strength, y width, z thin-behind ratio,
	//   w live-disturbance HIGH-WATER MARK (hw_drawinfo liveDisturb) -- NOT
	//     spare: FogSlabAt's gate, the disturbance loops and Ignite read it
	FVector4 mFogBow = { 0.f, 64.f, 0.6f, 0.f };

	// Second colour, mixed across the layer's own thickness. Cold at the
	// floor, warm at the top. w is how much of it to use; 0 keeps one colour.
	FVector4 mFogColor2 = { 0.7f, 0.5f, 0.35f, 0.f };

	// [BB] TEXTURE INSIDE THE GLOW -- see GlowTextureAt in main.fp. The wave
	// varies a glow's EDGE, which is no answer at all once coverage is high
	// enough that the edge is off screen. These happen inside the lit area.
	//   mGlowTex   x noise amount, y noise scale, z drift, w contrast
	//   mGlowTex2  x flow amount, y spacing, z speed, w sharpness
	//   mGlowTex3  x cell amount, y cell scale, z pulse speed, w vein width
	//   mGlowTex4  x disturbance reach, y state pulse depth, z state level,
	//              w pulse rate multiplier (GlowPulseRate, 1 = level's own rate)
	FVector4 mGlowTex = { 0.f, 0.02f, 1.f, 1.f };
	FVector4 mGlowTex2 = { 0.f, 64.f, 0.4f, 2.f };
	FVector4 mGlowTex3 = { 0.f, 96.f, 1.2f, 0.08f };
	FVector4 mGlowTex4 = { 0.f, 0.f, 0.f, 0.f };

	// [BB] WHAT SURVIVES THE COLOUR DRAIN.
	//
	// Desaturation used to be all or nothing: a monochrome preset was
	// monochrome, full stop, and blood came out the same grey as the wall it
	// was sprayed on. This makes the drain conditional on a colour's OWN
	// saturation, so a world can be grey and still keep the vivid things in it.
	// Sin City rather than Ingmar Bergman.
	//
	// It works because there is exactly ONE dodesaturate() in the shader and
	// every path goes through it -- textures, sprites, glow, sweeps, brightmaps,
	// the lot. A rule added there reaches all of them without a single call
	// site needing to know it exists.
	//
	//   x threshold: saturation above which colour survives (0 = old behaviour)
	//   y softness of that threshold
	//   z hue gate: 0 any hue, 1 red-dominant only, 2 green, 3 blue
	//   w spare

	FVector4 mDesatKeep = { 0.f, 0.15f, 0.f, 0.f };

	// [BB] SHAPES -- signed distance fields drawn onto surfaces.
	//
	// Sixteen rather than eight. Eight was the beam budget and it was chosen
	// for a system where every slot costs a segment solve per fragment; a
	// shape is a couple of ALU and an early reject, so the old cap was being
	// copied rather than reasoned about.
	//
	//   mShapeA[i]    xyz world centre (shader space), w size -- RESOLVED,
	//                 not the raw authored position: composed onto a parent
	//                 if linked, see hw_drawinfo.cpp's resolve loop
	//   mShapeB[i]    x kind + 16*orientation, y yaw (deg, resolved),
	//                 z thickness (ring/outline), w seam 0..1
	//   mShapeCol[i]  rgb colour, w intensity
	//   mShapeE[i]    x pitch (deg, resolved), y roll (deg, resolved) --
	//                 orient 3 only; z/w spare. See below.
	//
	//   kind  0 off, 1 disc, 2 ring, 3 square, 4 square outline,
	//         5 cross, 6 hexagon, 7 triangle
	//   orient 0 floor (upward faces), 1 walls (vertical faces), 2 any,
	//          3 STANDING -- freestanding in open air, not painted onto
	//          anything; full yaw/pitch/roll instead of a floor/wall
	//          decal's single in-plane rotation. See StandingShapesAt() in
	//          main.fp and the orientation/linking fields on ShapePos's
	//          neighbours in g_levellocals.h.
	//
	// The distance functions themselves are named and separate in main.fp
	// rather than inlined, so a later ZScript mirror is a transcription
	// instead of an excavation.
	FVector4 mShapeA[128];
	FVector4 mShapeB[128];
	FVector4 mShapeCol[128];

	// [BB] REPEAT -- one slot, many copies.
	//
	// A pattern cannot give each copy its own age or colour, which is what a
	// kill mark needs, so this does not replace the slots. What it does is
	// make ONE slot draw a formation, at any density, for the price of one --
	// and because the anchor is the slot's own position it still follows an
	// actor, so "eight runes orbiting this thing, spinning" is one slot and
	// fully dynamic.
	//
	//   x mode: 0 single, 1 radial, 2 grid
	//   y radial: how many. grid: how far the tiling reaches
	//   z radial: orbit radius. grid: tile spacing
	//   w spin (deg/sec) or drift (units/sec)
	FVector4 mShapeD[128];

	// x edge softness, y height fade, z glow reach past the edge,
	// w HOW MANY SLOTS ARE LIVE -- the loop runs to this, not to the cap.
	// Without it a 128-slot array would cost 128 iterations per fragment
	// whether three shapes existed or none did.
	FVector4 mShapeParams = { 2.f, 24.f, 0.f, 0.f };

	// What a seam reveals underneath. rgb, w unused.
	FVector4 mShapeUnder = { 1.f, 0.15f, 0.05f, 0.f };

	// [BB] THE FOG FOLLOWS THE ARCHITECTURE.
	//
	// A slab with one world Z for its top is flat across the whole map: knee
	// deep in one room, overhead in the pit next door, and it does not climb a
	// staircase. What people picture when they say "fog on the floor" is a
	// constant height ABOVE THE GROUND, which is a different question.
	//
	// ONE SIGNED NUMBER PER EDGE, and the sign picks the reference:
	//   0    absolute world Z, exactly as before
	//   > 0  follow the FLOOR, by this much
	//   < 0  follow the CEILING, by this much
	//
	// The magnitude is the gentleness, and it is the useful part. At 1 the
	// surface tracks every step exactly; at 0.3 it rises three units for every
	// ten the floor does, so it climbs a staircase as a slope rather than as a
	// flight of steps. A fog surface that steps looks like geometry; one that
	// lags looks like weather.
	//
	// Top follows floor is floor fog. Bottom follows ceiling is ceiling fog.
	// Both follow floor is a chest-high band that walks upstairs with you.
	//
	//   x top edge, y bottom edge, z floor under the eye, w ceiling over it
	//
	// The eye pair is pushed rather than derived: the plane uniforms describe
	// the FRAGMENT's sector, and using those for the eye end of the ray would
	// make fog swallow you the moment you looked at a wall on a higher floor.
	FVector4 mFogFollow = { 0.f, 0.f, 0.f, 0.f };


	// Upstream 5.0.0 thick-fog knobs. They are APPENDED here, after the last
	// FVector4, because that is where gl_shader.cpp's ViewpointUBO and
	// vk_shader.cpp's ViewpointData put uThickFogDistance/uThickFogMultiplier
	// -- a uniform block is matched by OFFSET, not by name order.
	float mThickFogDistance = -1.f;
	float mThickFogMultiplier = 30.f;

	// Two scalars past the last vec4 leave the struct 8 bytes short of a
	// 16-byte boundary. std140 rounds a block/struct size UP to a multiple of
	// 16 when it is used as an array element, and the Vulkan path declares
	// `ViewpointData viewpoints[2]` (one entry per eye), so without this pad
	// the C++ upload stride and the shader's array stride disagree and the
	// right eye reads the left eye's tail. Do not remove; do not declare these
	// in any of the GLSL copies -- std140 supplies the same padding implicitly.
	float mPadding1 = 0.f;
	float mPadding2 = 0.f;

	// [BB] STANDING SHAPE ORIENTATION -- resolved pitch/roll, once per frame,
	// same as mShapeD is for the repeat pattern. APPENDED HERE, after the
	// padding above rather than beside mShapeD, on purpose: mShapeD sits in
	// the middle of a block matched to the GLSL copies by OFFSET, and every
	// field after it (mShapeParams, mShapeUnder, mFogFollow, the thick-fog
	// pair, the padding) would shift if anything were inserted there. Adding
	// only at the true tail changes no existing offset at all. See
	// StandingShapesAt() in main.fp for what x/y hold; z/w are spare.
	//
	// Base yaw and world position for a standing shape do NOT get a new
	// field -- they are the resolved values already written into mShapeB.y
	// and mShapeA.xyz, which now carry rate and parent-link composition
	// baked in rather than the raw authored value. The shader was already
	// reading those two fields; nothing there had to change.
	FVector4 mShapeE[128];

	// [BB] THE ROOM THE SWEEP IS ALLOWED TO EXIST IN.
	//
	// The lattice is built from an INFINITE plane -- "perpendicular to X at
	// o.x" is a plane that exists at every Y and Z on the map -- so a window
	// looking anywhere toward it showed the grid standing in a room the sweep
	// had never entered. There is a radius, but the plane itself had no
	// extent, so this was not a leak to patch: the primitive had no concept
	// of a room at all.
	//
	//   mSweepRoomMin  xyz world min (shader space), w soft edge in units
	//   mSweepRoomMax  xyz world max (shader space), w 0 = unbounded
	//
	// w on Max is the switch rather than a separate flag: a room that was
	// never published leaves this zeroed and the shader skips the test
	// entirely, which is also the correct behaviour for every map that never
	// calls it.
	//
	// Outdoors needs no special case. An open area floods to a big box and
	// the grid fills it, which is what "if it is outdoor then it is outdoor"
	// asks for.
	FVector4 mSweepRoomMin;
	FVector4 mSweepRoomMax;

	// [STAMP] Surface stamps -- emissive shapes painted onto whatever real
	// surface is at a world point, blooming out of an impact and dying.
	//
	//   mStampPos[i]   xyz impact point (shader space), w radius
	//   mStampCol[i]   rgb colour 0..1,                 w progress 0..1
	//   mStampArg[i]   x shape id,                      yzw axis in world space
	//   mStampMod[i]   x surface-texture id,            y its strength
	//   mStampParams   x live count
	//
	// In the viewpoint block and not StreamData, for the same reason the beams
	// above are: these are scene-global, every surface in the level sees the
	// same list, and StreamData's size divides 64KB into MAX_STREAM_DATA draws.
	// Putting sixteen stamps there would have roughly halved the draws that fit
	// in one buffer to carry data that never varies per draw.
	//
	// It also means no gather pass. The original had to pick the nearest few
	// per wall and per flat because its slots rode the draw; here the shader
	// reads one global list and the flat and wall renderers are untouched.
	//
	// Appended at the tail in both GLSL copies in the same change. No longer the
	// LAST members: mLevelTime and mGpuParticleParams follow, and whatever is
	// added next goes after THEM. See mFogSurf above for what happens when the
	// three copies stop agreeing.
	//
	// [STAMP] 64 slots, raised from 16. Sixteen was running out in a real
	// firefight (every impact is a stamp, and eviction takes the oldest). The
	// shader loop breaks at the live count in mStampParams.x, so an empty slot
	// costs nothing per fragment; the price is 48 x 4 x 16 = 3,072 bytes of
	// block per eye. MAX_SURFACE_STAMPS in g_levellocals.h and
	// func_surfacestamps.fp, and both GLSL copies, say 64 in the same change.
	FVector4 mStampPos[64];
	FVector4 mStampCol[64];
	FVector4 mStampArg[64];
	FVector4 mStampMod[64];
	FVector4 mStampParams = { 0.f, 0.f, 0.f, 0.f };

	// [GPUPARTICLES] THE CLOCK AND THE PARTICLE KNOBS.
	//
	//   mLevelTime          x WORLD seconds at render rate, FLevelLocals::WorldSeconds
	//                       y REAL seconds at render rate, FLevelLocals::RealSeconds
	//                         zw spare
	// RS FORK -- WORLD CLOCK: x was (maptime + TicFrac) / TICRATE and still is that
	// exact number with no slow motion running. When the world is slowed, x stretches
	// with it and y does not, so an effect that follows the player's body has a clock
	// to age on. "Engine docs/SLOWMO_PLAN.md".
	//   mGpuParticleParams  x size scale, y max size (map units),
	//                       z stretch scale, w intensity scale
	//
	// mLevelTime is general and named for what it holds: the `timer` uniform
	// cannot stand in for it (wall-clock, scaled by the bound material, zero
	// without one, runs while paused). Stateless GPU particles age by it; stamps,
	// shapes and disturbances could move their aging onto it too.
	// mGpuParticleParams carries the r_gpuparticles_* live-tuning cvars.
	//
	// APPENDED LAST, and appended last in both GLSL copies (gl_shader.cpp's
	// ViewpointUBO, vk_shader.cpp's ViewpointData plus its #defines) in the same
	// change. No longer the last members: mBeamLook follows, and whatever is
	// added next goes after IT.
	FVector4 mLevelTime = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mGpuParticleParams = { 1.f, 8.f, 1.f, 1.f };

	// [BEAMLINES] THE LOOK OF EACH UPLOADED BEAM LINE. See mBeamA above and
	// "Engine docs/BEAM_LINES_PLAN.md".
	//
	//   mBeamLook[u]  x air glow, y halo strength, z taper, w impact flare
	//
	// u is the COMPACTED upload index, the one mBeamA/B/Col use. hw_drawinfo.cpp
	// fills it from the slot's own SetBeamStyle, or from the scene values
	// (BeamAirGlow, BeamGlow, BeamTaper, BeamFlare) when it has none -- the very
	// numbers main.fp used to read from mBeamParams.y/.w and mBeamFX.z/.w -- so
	// an unstyled line is drawn exactly as before. With r_beams_drawn routing a
	// line to the drawn path, x is 0 and the per-pixel air loop skips it.
	//
	// +2,048 bytes per viewpoint: 22,512 -> 24,560. See the bind-range assert
	// at the bottom of this file for why that is checked TWICE over.
	//
	// APPENDED LAST, and last in both GLSL copies (gl_shader.cpp's ViewpointUBO,
	// vk_shader.cpp's ViewpointData plus its #define) in the same change.
	// Anything added after this goes after IT, in all three places.
	// (No longer the last members: [round2 B2] mSweepPassed follows.)
	FVector4 mBeamLook[128];

	// [round2 B2] THE PASSED-REGION LOOK. See FLevelLocals::SweepBandPassed and
	// SweepPassedAt in main.fp.
	//
	//   mSweepPassed       x tint mix, y darken, z desaturate, w soft (map units)
	//   mSweepPassedColor  rgb tint, w enable -- 1 only when a live band has its
	//                      passed bit AND some look term is non-zero, so the
	//                      shader pays one compare when nothing asks for it
	//
	// +32 bytes per viewpoint: 24,560 -> 24,592. APPENDED LAST, after mBeamLook,
	// so no existing member moves (mBeamA, mBeamParams and mBeamLook keep their
	// offsets), and last in both GLSL copies (gl_shader.cpp's ViewpointUBO,
	// vk_shader.cpp's ViewpointData plus its #defines) in the same change.
	// Anything added after these goes after THEM, in all three places.
	FVector4 mSweepPassed = { 0.f, 0.f, 0.f, 32.f };
	FVector4 mSweepPassedColor = { 1.f, 1.f, 1.f, 0.f };

	// [2a] READABLE SCENE DEPTH -- see "Engine docs/GPU_PARTICLES_STAGE2_PLAN.md"
	// 2a and FRenderState::SetSceneDepthReadable.
	//
	//   mLinearizeDepth      x LinearizeDepthA, y LinearizeDepthB: the pair the
	//                        post passes use (lineardepth.fp, volumetricbeam.fp),
	//                        distance along the view axis = 1 / (raw * A + B).
	//                        zw spare.
	//   mGpuParticleParams2  x soft distance in map units (r_gpuparticles_soft,
	//                        0 = hard edges, as before 2a); [2d] y which output
	//                        gpuparticles.fp writes, and so which blend
	//                        RenderTranslucent draws the ring with: 0 STYLE_Add,
	//                        exactly as before 2d; 1 premultiplied alpha, while a
	//                        particle whose definition occludes is alive
	//                        (HWDrawInfo::StartScene); zw spare.
	//
	// mLinearizeDepth is general, named for what it holds: any forward effect that
	// reads scene depth through the read-only depth pass linearizes with it --
	// gpuparticles.fp now, drawnlines.fp later. Both are filled beside
	// mGpuParticleParams in HWDrawInfo::StartScene.
	//
	// +32 bytes per viewpoint: 24,592 -> 24,624. APPENDED LAST, after
	// mSweepPassedColor, so no existing member moves, and last in both GLSL copies
	// (gl_shader.cpp's ViewpointUBO, vk_shader.cpp's ViewpointData plus its
	// #defines) in the same change. Anything added after these goes after THEM,
	// in all three places.
	FVector4 mLinearizeDepth = { 0.f, 1.f, 0.f, 0.f };
	FVector4 mGpuParticleParams2 = { 0.f, 0.f, 0.f, 0.f };

	void CalcDependencies()
	{
		mNormalViewMatrix.computeNormalMatrix(mViewMatrix);
	}
};

static_assert((sizeof(HWViewpointUniforms) % 16) == 0, "HWViewpointUniforms must remain 16-byte aligned for std140 array stride.");

// [STAMP] One viewpoint is bound as ONE uniform block (hw_viewpointbuffer.cpp
// aligns and binds per block), so the whole struct has to fit inside the
// device's maxUniformBufferRange. This block has grown a lot -- 128-slot beam
// and shape arrays, then stamps -- and the failure mode if it ever goes over is
// a device-dependent runtime bind failure rather than anything a compiler would
// have said. So it is asserted here, where the next person adding an array will
// trip it at build time instead of on somebody else's GPU.
//
// 65536 is the value this project's target hardware reports. Vulkan's own
// guaranteed minimum is only 16384, so a block this size already assumes a
// desktop GPU -- which this fork does anyway.
static_assert(sizeof(HWViewpointUniforms) <= 65536, "HWViewpointUniforms exceeds the uniform block range it is bound with.");

// [BEAMLINES] AND TWICE OVER, because that is what Vulkan actually binds. The
// descriptor at set 1 binding 0 covers GetBlockSize() * 2 (vk_descriptorset.cpp,
// UpdateHWBufferSet) so a multiview pass can index viewpoints[0] and [1], and
// GetBlockSize() is sizeof rounded UP to the device's uniform offset alignment
// (hw_viewpointbuffer.cpp). The check above only ever guarded one copy.
//
// Rounded here to 256, the largest minUniformBufferOffsetAlignment the Vulkan
// spec allows, so it holds on any device: with mBeamLook the block is 24,560
// bytes, aligned 24,576, bound 49,152 of 65,536. [round2 B2] With mSweepPassed
// and mSweepPassedColor it is 24,592, aligned 24,832, bound 49,664. [2a] With
// mLinearizeDepth and mGpuParticleParams2 it is 24,624, still aligned 24,832,
// bound 49,664.
static_assert(((sizeof(HWViewpointUniforms) + 255) / 256) * 256 * 2 <= 65536,
	"Two aligned HWViewpointUniforms blocks exceed the 65,536-byte range Vulkan binds them with.");
