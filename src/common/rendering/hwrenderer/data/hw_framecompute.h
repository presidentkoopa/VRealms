/*
** hw_framecompute.h
**
** [COMPUTE] What the game side hands the renderer backend once per frame for its GPU
** compute work (DFrameBuffer::RunFrameCompute).
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: compute work (the smoke volume first -- "Engine docs/
** SMOKE_VOLUME_PLAN.md" #13) needs to know about the level -- where the eye is,
** whether the world clock advanced, whether the map changed -- but the common
** renderer and the Vulkan backend never see FLevelLocals. So the game side
** (PrepareFrameCompute, src/rendering/hwrenderer/hw_entrypoint.cpp) reads the
** level and fills this plain struct, and the backend acts on it. That also keeps
** the backend replaceable: a render rebuild re-implements RunFrameCompute and
** keeps everything that fills this.
**
** One member per compute client. The level field (#8), the debris pool (#9), the
** impulse field (#12) and surface damage (#17) add theirs beside Smoke.
**
** Presentation only: nothing here is ever read back into the playsim.
**
*/

#pragma once

#include <cstdint>
#include "hw_emissivevolumeframe.h"	// [EMISSIVEVOLUMES] EmissiveVolumeFrame

// [SMOKEVOLUME] One smoke grid quality (r_smoke_quality): cells per axis and the
// cell size in map units. Doom axes: x, y horizontal, z up.
struct SmokeGridSpec
{
	int SizeX = 0;
	int SizeY = 0;
	int SizeZ = 0;
	int CellSize = 0;

	uint64_t Cells() const { return (uint64_t)SizeX * (uint64_t)SizeY * (uint64_t)SizeZ; }
};

// Bytes per cell of the volume the backend allocates (the plan's table): density and
// heat RG16F as a ping-pong pair (2 x 4), velocity RGBA16F as a pair (2 x 8), and the
// R8 solid mask (1). The pair holds the last two simulation states, which is what the
// march blends by TicFrac for smoothing (13b/13c) -- nothing extra is stored for it.
// The light grid (13d) is a separate allocation.
inline constexpr int SMOKE_BYTES_PER_CELL = 25;

inline constexpr int SMOKE_QUALITY_MIN = 1;
inline constexpr int SMOKE_QUALITY_DEFAULT = 2;
inline constexpr int SMOKE_QUALITY_MAX = 4;

// Resolution and area together, per the owner's answer 1 (about 150 MB at default,
// finer and bigger than the plan's first 20 MB grid). Every size is a multiple of 8,
// the compute shaders' local size.
//   1: 192 x 192 x  64 at 10 units = 1920 x 1920 x  640   about  59 MB
//   2: 256 x 256 x  96 at  8 units = 2048 x 2048 x  768   about 157 MB (default)
//   3: 320 x 320 x 128 at  8 units = 2560 x 2560 x 1024   about 328 MB
//   4: 384 x 384 x 128 at  8 units = 3072 x 3072 x 1024   about 472 MB
inline SmokeGridSpec SmokeGridForQuality(int quality)
{
	static const SmokeGridSpec specs[SMOKE_QUALITY_MAX - SMOKE_QUALITY_MIN + 1] =
	{
		{ 192, 192,  64, 10 },
		{ 256, 256,  96,  8 },
		{ 320, 320, 128,  8 },
		{ 384, 384, 128,  8 },
	};
	if (quality < SMOKE_QUALITY_MIN) quality = SMOKE_QUALITY_MIN;
	if (quality > SMOKE_QUALITY_MAX) quality = SMOKE_QUALITY_MAX;
	return specs[quality - SMOKE_QUALITY_MIN];
}

// [SMOKEVOLUME] 13b: THE UNITS THE SIMULATION AND THE DRAWING SHARE.
//
// Density 1 at absorption 1 (FLevelLocals::SmokeLook) has this extinction: light
// through 64 map units of it keeps e^-1, about a third. 13c's march uses exactly this,
// so what EmitSmoke's documentation promises ("amount 1 is thick haze") stays true.
inline constexpr float SMOKE_EXTINCTION_PER_MAP_UNIT = 1.0f / 64.0f;

// The simulation's tiles: SMOKE_TILE_CELLS cells a side (the compute shaders' local
// size). Every grid size is a whole number of tiles, and the box only ever moves by
// whole tiles, so a tile always covers the same piece of the world. A tile with no
// smoke, heat or motion in or next to it is skipped by the step (smoke_tiles.comp).
inline constexpr int SMOKE_TILE_CELLS = 8;

// The most solid-mask bytes one frame copies in (SmokeMaskUpload): the CPU side stops its
// rasterisation there, and the backend's staging buffer is this big. 4 MiB is about 40
// mask tiles at the default quality, far more than the CPU budget does in a frame.
inline constexpr size_t SMOKE_MASK_UPLOAD_BYTES_PER_FRAME = size_t(4) << 20;

// Below these a cell counts as empty: the tile pass and the CPU's bounds use them.
inline constexpr float SMOKE_EMPTY_DENSITY = 1.0f / 1024.0f;
inline constexpr float SMOKE_EMPTY_HEAT = 1.0f / 256.0f;
inline constexpr float SMOKE_EMPTY_VELOCITY = 1.0f / 128.0f;	// cells per step

// [SMOKEVOLUME] One kernel the GPU applies to a box of cells at the start of a step,
// before advection (shaders/compute/smoke_inject.comp): an EmitSmoke, a CarveSmoke or
// a PushEffectImpulse, converted by the CPU side (hw_smokevolume.cpp). Positions are
// grid-relative CELLS -- cell (i, j, k) spans [i, i + 1) on each axis, so its centre is
// at i + 0.5 -- and speeds are cells per step. The shape is a capsule from Start to End
// (Start == End: a ball). A long capsule is split into several kernels whose regions
// are disjoint slabs, each still carrying the whole capsule, so no cell is changed
// twice and none is missed.
struct SmokeKernel
{
	enum : int32_t { EMIT = 0, CARVE = 1, IMPULSE = 2 };

	int32_t Kind = EMIT;
	int32_t Step = 0;				// which of this frame's steps it belongs to, 0..Steps-1
	int32_t RegionMin[3] = { 0, 0, 0 };	// first cell of the box it may touch, inside the grid
	int32_t RegionMax[3] = { 0, 0, 0 };	// one past the last
	float Start[3] = { 0, 0, 0 };
	float End[3] = { 0, 0, 0 };
	float Radius = 0;				// cells; never below SmokeKernel::MIN_RADIUS
	float Amount = 0;				// EMIT: density added at the centre; CARVE: share removed at the centre, 0..1
	float Heat = 0;					// EMIT: heat added at the centre
	float Strength = 0;				// IMPULSE: cells per step outward at the centre (negative: inward)
	float Velocity[3] = { 0, 0, 0 };	// EMIT: cells per step added at the centre
	float Soot = 0;					// [13e] EMIT: soot density added at the centre (Amount x the emit's soot share), 0 = none

	// A kernel thinner than this would fall between cell centres. It is widened to it,
	// and an emit's amount (a carve's) is scaled by the volume (cross-section) it lost,
	// so a tiny puff adds a tiny amount rather than a full cell of smoke.
	static constexpr float MIN_RADIUS = 0.75f;
};

// [SMOKEVOLUME] One block of solid-mask texels to copy into the mask volume: Size[0] x
// Size[1] x Size[2] bytes at MaskBytes + Offset, x fastest, then y, then z (a 3D
// image's texel order), 255 solid / 0 open, landing with its first texel at grid cell
// Min. Rasterised by SH1's LevelSolidity on the CPU.
struct SmokeMaskUpload
{
	int32_t Min[3] = { 0, 0, 0 };
	int32_t Size[3] = { 0, 0, 0 };
	size_t Offset = 0;
};

// [SMOKEVOLUME] The simulation's numbers for this frame's steps, from the level's smoke
// look, its wind and the renderer-read cvars (hw_smokevolume.cpp). Per STEP (one world
// tic), in cells.
struct SmokeSimSettings
{
	float DensityKeep = 1;			// density multiplier per step (dissipation)
	float HeatKeep = 1;				// heat multiplier per step (cooling)
	float VelocityKeep = 1;			// velocity multiplier per step (damping)
	float Diffusion = 0;			// share blended toward the neighbourhood per step, 0..1
	float HeatLift = 0;				// cells per step added to upward velocity per step, per unit of heat
	float DensityLift = 0;			// the same per unit of density (counted up to 1)
	float Wind[3] = { 0, 0, 0 };	// cells per step every cell drifts, walls permitting
	float Turbulence = 0;			// cells per step of swirl (divergence-free noise over the world)
	float TurbulenceFrequency = 0;	// swirl noise cells per grid cell
	float MaxDisplacement = 4;		// a step never moves anything further than this, cells
	float MaxSpeed = 4;				// the velocity field is clamped to this, cells per step
	// [13e] 1 while soot may be in the volume (the CPU's soot bound is above empty), else 0. Soot is darkness-weighted
	// density carried in the velocity image's w (smoke_inject.comp, smoke_advect.comp); at 0 nothing reads or carries
	// it, and w stays exactly 0 -- the volume is exactly 13d's.
	float SootLive = 0;
};

// [SMOKEVOLUME] 13d: THE LIGHT GRID ("Engine docs/SMOKE_VOLUME_PLAN.md" 13d, owner answer 4).
//
// Evaluating every light at every march sample is far too much at VR frame rates, so the light is
// worked out once a frame on a grid over the smoke box -- the method shipping volumetric fog uses --
// and the march (shaders/pp/smokemarch.fp) samples it. The grid is world-aligned, so both eyes read
// the same light. Its resolution is LIGHT CELLS PER SMOKE TILE (r_smoke_light_quality), so a light
// cell boundary always falls on a world position the box's whole-tile recentres keep:
//   1: 4 per tile, half the smoke grid's resolution (128 x 128 x 48 at the default smoke quality)
//   2: 6 per tile, three quarters (192 x 192 x 72), the default
//   3: 8 per tile, the smoke grid's own (256 x 256 x 96)
// Two volumes, SMOKE_LIGHT_BYTES_PER_CELL a cell: RGBA16F, the light reaching the cell (rgb) and its
// luminance weight (a); RGBA8 SNORM, the direction light travels there averaged by weight (xyz, whose
// length is the share of the light that comes from that direction). Plus a 2D RGBA8 map of each
// column's ambient (sector) light. Filled by shaders/compute/smoke_light.comp (vk_smokevolume.cpp).
inline constexpr int SMOKE_LIGHT_QUALITY_MIN = 1;
inline constexpr int SMOKE_LIGHT_QUALITY_DEFAULT = 2;
inline constexpr int SMOKE_LIGHT_QUALITY_MAX = 3;
inline constexpr int SMOKE_LIGHT_BYTES_PER_CELL = 12;

// The most lights one frame puts into the grid: the view light list's capacity (stage 2d).
inline constexpr int SMOKE_LIGHTS_MAX = 32;

struct SmokeLightGridSpec
{
	int SizeX = 0;
	int SizeY = 0;
	int SizeZ = 0;
	int CellsPerTile = 0;	// light cells per SMOKE_TILE_CELLS smoke cells, on each axis
	double CellSize = 0;	// map units

	uint64_t Cells() const { return (uint64_t)SizeX * (uint64_t)SizeY * (uint64_t)SizeZ; }
};

inline SmokeLightGridSpec SmokeLightGridFor(const SmokeGridSpec& grid, int lightQuality)
{
	static const int cellsPerTile[SMOKE_LIGHT_QUALITY_MAX - SMOKE_LIGHT_QUALITY_MIN + 1] = { 4, 6, 8 };
	if (lightQuality < SMOKE_LIGHT_QUALITY_MIN) lightQuality = SMOKE_LIGHT_QUALITY_MIN;
	if (lightQuality > SMOKE_LIGHT_QUALITY_MAX) lightQuality = SMOKE_LIGHT_QUALITY_MAX;
	SmokeLightGridSpec spec;
	spec.CellsPerTile = cellsPerTile[lightQuality - SMOKE_LIGHT_QUALITY_MIN];
	// Every smoke grid size is a whole number of tiles.
	spec.SizeX = grid.SizeX / SMOKE_TILE_CELLS * spec.CellsPerTile;
	spec.SizeY = grid.SizeY / SMOKE_TILE_CELLS * spec.CellsPerTile;
	spec.SizeZ = grid.SizeZ / SMOKE_TILE_CELLS * spec.CellsPerTile;
	spec.CellSize = (double)grid.CellSize * SMOKE_TILE_CELLS / spec.CellsPerTile;
	return spec;
}

// [13d] One light as the grid takes it, from Level->lights (hw_smokevolume.cpp). Doom axes, map units,
// positions from the smoke box's minimum corner.
struct SmokeLightRecord
{
	float Position[3] = { 0, 0, 0 };
	float Radius = 0;
	float Color[3] = { 0, 0, 0 };			// rgb x the look's scatter; negative for a subtractive light
	float Weight = 0;						// its luminance (0 for a subtractive light): what it counts toward the direction
	float SpotDirection[3] = { 0, 0, 0 };	// main.fp's spot direction (the cone's axis pointing back at the light)
	float SpotCosOuter = -2;				// a point light keeps -2 and -1, which the cone test lets everything through
	float SpotCosInner = -1;
	int ShadowRow = -1;						// its row in the engine's shadow map this frame; -1 = not occluded
};

// [EFFECTLIGHTS] LD: EFFECT LIGHTS IN THE SMOKE ("Engine docs/EFFECT_LIGHTS_LD_IMPL_NOTES.md", LIGHTS_20_21_22_PLAN.md 2h).
//
// Effect lights (hw_effectlights.h: sparks, embers, impacts, tracers -- points and line segments, sorted into world bins
// around the eye once a frame) light the grid through smoke_light.comp's PASS 2: ONE dispatch over the light cells any of
// them can reach, each cell looping only its own effect-light bin, from the same two storage buffers surfaces read. The
// CPU side (SmokeVolume::GatherEffectLights) decides it from EffectLights::Get().FrameBins(); LightCount 0 means no pass 2
// this frame, and then the grid is filled exactly as without effect lights. The dynamic lights keep pass 1.
struct SmokeEffectLightPass
{
	int LightCount = 0;					// binned effect lights (and residual glows) whose reach meets a light cell's centre; 0 = none
	int RegionMin[3] = { 0, 0, 0 };		// the light cells their reach covers (union of boxes, clipped to the grid), Doom axes
	int RegionMax[3] = { 0, 0, 0 };		// one past the last
	float BinOffset[3] = { 0, 0, 0 };	// the light grid's corner minus the effect-light grid's corner, Doom axes, map units (whole)
	float Scatter = 0;					// the look's scatter, 0..1, as pass 1's colours take it
};

// [13F] SURFACE LIGHT IN THE SMOKE ("Engine docs/SMOKE_13F_IMPL_NOTES.md"; NEXT_ENGINE_QUEUE 13f with the glow lane's G4 and G5).
//
// What a room's surfaces and its grading do to the light in its air, in the light grid's ambient pass (pass 0), per cell:
//   - the column's sector light (13d's bytes) through the DARKNESS CURVE (FLevelLocals::Dark*, main.fp's DarknessAt: the curve per
//     sector, the distance and height terms per cell) and the sweeps' PASSED LOOK (main.fp's SweepPassedAt) -- r_smoke_darkness;
//   - plus the GLOW LANES of the column's sector (sector_t's wall glow as sector_t::GetWallGlow resolves it -- a texture's GLDEFS
//     glow when GlowColor is 0 -- measured from the column's floor and ceiling planes; its flat glow at the column, fading with
//     height over the same reach), recoloured by recolour bands, and the SWEEP BANDS' light (add, lift, crush) -- r_smoke_surfaceglow.
// The dynamic and effect lights (passes 1 and 2) are not graded: main.fp grades only the room's light, and adds lights, glow
// and bands after it. Presentation only; the CPU side reads the level and writes nothing back.
//
// Carried by smoke_light.comp's SMOKE_SURFACE_LIGHT variant of pass 0, from two storage buffers, only on frames Live says so:
// any other frame dispatches pass 0's own program exactly as before. Both buffers are float32 (the ints in them bit-copied):
//   COLUMNS  ivec4 (light-grid columns x, y, 0, 0), then per column x fastest vec2 (its sector record, -1 = the fallback's; its
//            distance to its sector's nearest edge, map units, -1 = not worked out yet). Renewed only when a column changes.
//   RECORDS  SMOKE_SURFACE_HEADER_VEC4S header vec4s (smoke_light.comp's SmokeSurfaceSSO lists them), then the records -- the
//            lump's surfaceRecords[], so EVERY RECORD INDEX COUNTS FROM THE FIRST RECORD AFTER THE HEADER (a column's, the
//            fallback's, a glow record's): one vec4 per sector record (x the darkness curve or -1, y its glow record's first vec4
//            or -1, z its colormap desaturation 0..1), the fallback's last, then SMOKE_SURFACE_GLOW_VEC4S vec4s per glowing
//            sector. Every Live frame.
// Positions are Doom axes, map units from the light grid's minimum corner.
inline constexpr int SMOKE_SURFACE_BANDS = 8;										// FLevelLocals::MAX_SWEEP_BANDS
inline constexpr int SMOKE_SURFACE_HEADER_VEC4S = 7 + 4 * SMOKE_SURFACE_BANDS;
inline constexpr int SMOKE_SURFACE_GLOW_VEC4S = 14;

// [13F] This frame's surface light, decided on the CPU (hw_smokevolume.cpp, SmokeVolume::UpdateSurfaceLight).
struct SmokeSurfaceLightFrame
{
	bool Live = false;						// pass 0 takes the SMOKE_SURFACE_LIGHT variant this frame
	const float* Columns = nullptr;			// the COLUMNS buffer (above), ColumnFloats floats
	size_t ColumnFloats = 0;
	uint64_t ColumnSerial = 0;				// renewed whenever the columns change: the backend copies them in when it differs
	const float* Records = nullptr;			// the RECORDS buffer (above), RecordFloats floats
	size_t RecordFloats = 0;
};

// [13d] This frame's light grid, decided on the CPU (hw_smokevolume.cpp).
struct SmokeLightFrame
{
	int Quality = 0;						// r_smoke_light_quality; 0 = no grid
	SmokeLightGridSpec Grid;
	float AmbientScale = 0;					// the look's ambient
	const SmokeLightRecord* Lights = nullptr;
	int LightCount = 0;
	// Grid.SizeX x Grid.SizeY texels, RGBA8, x fastest: each column's sector light times its colour.
	const uint8_t* AmbientColumns = nullptr;
	size_t AmbientByteCount = 0;
	uint64_t AmbientSerial = 0;				// renewed whenever those bytes change: the backend copies them in when it differs
	SmokeEffectLightPass EffectLights;		// [EFFECTLIGHTS] LD: pass 2 over the effect-light bins (LightCount 0 = none)
	SmokeSurfaceLightFrame Surface;			// [13F] surface glow, sweep bands, darkness and the passed look in pass 0 (Live false = none)
};

// [SMOKEVOLUME] 13e: BEAMS IN THE SMOKE ("Engine docs/SMOKE_VOLUME_PLAN.md" 13e, "Engine docs/SMOKE_13E_IMPL_NOTES.md").
//
// The beam lines (FLevelLocals' beam slots: the grab lasers, the Lance, and the same slots when r_beams_drawn routes
// them to the drawn-line path) whose light may meet the smoke this frame, resolved exactly as the per-pixel upload
// resolves them (hw_drawinfo.cpp, ResolveBeamLine), the nearest SMOKE_BEAMS_MAX to the eye. World-aligned, so one list
// serves both eyes: the backend copies it into a small image (VkSmokeVolume, PPExternalImage::SmokeBeams) that the smoke
// drawing reads -- the light a beam scatters in the smoke, and a beam's own glow kept from being dimmed by haze behind it.
inline constexpr int SMOKE_BEAMS_MAX = 16;
inline constexpr int SMOKE_BEAM_TEXELS = 4;			// texels a beam takes in the image: one row each (below)

// One beam as the drawing takes it. Positions in GL axes (map x, map z, map y), map units from the smoke grid's minimum
// corner, so the numbers stay small. The image holds, per beam column i, row 0 A + Thick, row 1 B + Soft, row 2 Color +
// Intensity, row 3 Look.
struct SmokeBeamRecord
{
	float A[3] = { 0, 0, 0 };		// the start (the muzzle)
	float Thick = 0;				// the hot core, map units
	float B[3] = { 0, 0, 0 };		// the end (the impact)
	float Soft = 0;					// how far the halo reaches past the core, map units
	float Color[3] = { 0, 0, 0 };	// 0..1
	float Intensity = 0;
	float Look[4] = { 0, 0, 0, 0 };	// x air glow, y halo, z taper, w flare: the slot's own style, or the scene look
};

// [13e] This frame's beam list, decided on the CPU (hw_smokevolume.cpp, only on frames with smoke to draw).
struct SmokeBeamFrame
{
	const SmokeBeamRecord* Beams = nullptr;
	int Count = 0;					// 0..SMOKE_BEAMS_MAX
};

// [SMOKEVOLUME] This frame's smoke volume, decided on the CPU (hw_smokevolume.cpp).
struct SmokeVolumeFrame
{
	// A frame never runs more steps than this, so a hitch does not become a burst
	// (review S7). A bigger maptime jump drops the backlog.
	static constexpr int MAX_STEPS_PER_FRAME = 2;

	// The volume should exist: r_smoke is on AND something asked for smoke on this map
	// within the linger time, or smoke is still in the air -- a mod's EmitSmoke /
	// CarveSmoke, or the test source r_smoke_computetest. False frees the images.
	bool Active = false;

	int Quality = SMOKE_QUALITY_DEFAULT;
	SmokeGridSpec Grid;

	// The grid box's minimum corner in whole cells: world position = cell x
	// Grid.CellSize. World-aligned, always a multiple of SMOKE_TILE_CELLS, so both eyes,
	// every frame and every tile agree on a cell.
	int OriginCell[3] = { 0, 0, 0 };

	// Simulation steps this frame, 0..MAX_STEPS_PER_FRAME: how far Level->maptime
	// advanced since the last frame that stepped.
	int Steps = 0;

	// Level->maptime this frame, and where the frame sits between the last two
	// steps. [13c] the march blends the pair's two states by TicFrac.
	int MapTime = 0;
	float TicFrac = 0.f;

	// Renewed on every map change and savegame load: the backend clears the volume's
	// contents (the old map's smoke is gone) but keeps the allocation.
	uint64_t LevelSerial = 0;

	// ---- [13b] the simulation. The backend does these in this order. ----

	// 1. A NEW BOX (first placement, quality change, new map, a jump too far to shift, or
	//    the backend reported a mask it did not initialise for BoxEpoch): density, heat,
	//    velocity and the tile maps are zeroed and the mask is set ALL SOLID -- nothing
	//    flows until the CPU's rasterisation opens it, tile by tile. BoxEpoch rises with
	//    every new box; the backend reports it back (SmokeVolumeBackendStatus::MaskEpoch).
	bool NewBox = false;
	uint64_t BoxEpoch = 0;

	// 2. A RECENTRE: new origin minus old, whole tiles on each axis. The contents (smoke,
	//    velocity, mask) move so each cell keeps its world position; what comes into the
	//    box is empty air over a solid mask until rasterised.
	int ShiftCells[3] = { 0, 0, 0 };

	// 3. Empty density, heat and velocity (ClearSmoke, or the volume has gone quiet).
	bool ClearContents = false;

	// 4. Copy these mask blocks in.
	const SmokeMaskUpload* MaskUploads = nullptr;
	int MaskUploadCount = 0;
	const uint8_t* MaskBytes = nullptr;
	size_t MaskByteCount = 0;

	// 5. Steps simulation steps when Simulate (false: the CPU's bounds say nothing is left
	//    that could show or move, so no step is recorded). Step i applies the kernels
	//    whose Step is i, then advects, then rebuilds the tile maps.
	bool Simulate = false;
	const SmokeKernel* Kernels = nullptr;
	int KernelCount = 0;
	SmokeSimSettings Sim;

	// [13c] Visible smoke may exist (the CPU's density bound is above empty): draw it.
	bool HasSmoke = false;

	// [13d] The light grid: its quality whenever the volume is active, its lights and ambient columns on
	// frames with smoke to draw. The backend fills the grid after the steps, only when HasSmoke.
	SmokeLightFrame Light;

	// [13e] The beams that may meet the smoke, on frames with smoke to draw (Count 0 otherwise). The backend copies them
	// into its beam list image when HasSmoke.
	SmokeBeamFrame Beams;
};

// [SMOKEVOLUME] What the backend did with the smoke volume, for the CPU side's NEXT
// frame (hw_smokevolume.cpp). Renderer-internal, written by the backend
// (vk_smokevolume.cpp) and read by the game-side renderer code; nothing here ever
// reaches the playsim. It is how the CPU side learns that the volume was allocated
// again underneath it (a device reset, a late allocation) and its mask must be built
// again, or that this device refused a quality.
struct SmokeVolumeBackendStatus
{
	bool Allocated = false;
	int Quality = 0;
	int RefusedQuality = 0;		// a quality this device refused until smoke stops being asked for; 0 = none
	uint64_t MaskEpoch = 0;		// the BoxEpoch of the NewBox that initialised the current mask; 0 = none did
	int LightQuality = 0;			// [13d] the light grid the backend holds (r_smoke_light_quality); 0 = none
	int RefusedLightQuality = 0;	// [13d] a light quality this device refused until it changes; 0 = none
	int BeamCount = 0;				// [13e] beams the backend's beam list image holds for this frame's drawing; 0 = none (or no image)
	// [SMOKELIGHTCULL] E6 (hw_smoketilecover.h): this frame's light grid fill -- 0 none (no smoke to draw), 1 filled, 2 skipped
	// (nothing it reads changed since the last fill) -- the light cells its dispatches covered, and the cells the whole fill
	// covers (the grid, every light's box, the effect lights' region). For the perf log.
	int LightFill = 0;
	uint64_t LightCells = 0;
	uint64_t LightCellsUncut = 0;
};

inline SmokeVolumeBackendStatus& SmokeVolumeStatus()
{
	static SmokeVolumeBackendStatus status;
	return status;
}

//-----------------------------------------------------------------------------
//
// [LEVELFIELD] #8 PARTICLE COLLISION WITH THE WHOLE LEVEL ("Engine docs/
// COLLISION_DEBRIS_MESH_PLAN.md" 8, "Engine docs/COLLISION_8_IMPL_NOTES.md").
//
// A signed distance field around the eye that particles sample: negative inside
// solid, in map units, clamped to a band. Two levels -- a fine one close by, a
// coarse one further out. The CPU side (hw_levelfield.cpp) decides where each
// level's window is, which tiles need (re)baking, and writes each tile's column
// part from SH1's solidity (sign) and the CURRENT floor and ceiling heights; the
// backend (vk_levelfield.cpp, shaders/compute/field_bake.comp) copies that in and
// takes the minimum with every nearby line's wall pieces, also from the current
// planes. Nothing that can move comes from the load-time level mesh (review D1).
//
// TOROIDAL ADDRESSING. A texel holds the world cell congruent to it modulo the
// volume's size (every size is whole tiles). A window move shifts nothing: the
// tiles whose world meaning changed are invalidated (g = 0) and baked again. A
// sample is trusted only inside its level's window and where g says baked.
//
// THE HEADER. Each frame's windows travel with the field as a tiny image written
// by a GPU command in the same command stream as the invalidations and bakes, so
// every draw -- whenever it was recorded -- reads a header and texels that agree.
// gpuparticles.vp reads all three through the fixed set (vk_descriptorset.cpp,
// bindings 5, 6 and 9).
//
// Presentation only: nothing here is read back into the playsim.
//
//-----------------------------------------------------------------------------

inline constexpr int LEVEL_FIELD_LEVELS = 2;			// 0 fine, 1 coarse
inline constexpr int LEVEL_FIELD_TILE_CELLS = 16;		// a tile is this many cells a side; every size is whole tiles
inline constexpr int LEVEL_FIELD_TILE_TEXELS = LEVEL_FIELD_TILE_CELLS * LEVEL_FIELD_TILE_CELLS * LEVEL_FIELD_TILE_CELLS;
inline constexpr int LEVEL_FIELD_BYTES_PER_CELL = 4;	// RG16F: r signed distance, g 1 = baked
inline constexpr int LEVEL_FIELD_HEADER_TEXELS = 3;		// the header image is 3 x 1, RGBA32F

inline constexpr int LEVEL_FIELD_QUALITY_MIN = 1;
inline constexpr int LEVEL_FIELD_QUALITY_DEFAULT = 2;
inline constexpr int LEVEL_FIELD_QUALITY_MAX = 3;

// One level of the field at one quality (r_particlecollision_quality). Doom axes: x, y
// horizontal, z up.
struct LevelFieldSpec
{
	int SizeX = 0;
	int SizeY = 0;
	int SizeZ = 0;
	int CellSize = 0;		// map units
	int BandCells = 0;		// distances are exact up to this many cells; further is clamped to it

	uint64_t Cells() const { return (uint64_t)SizeX * (uint64_t)SizeY * (uint64_t)SizeZ; }
	double Band() const { return (double)BandCells * (double)CellSize; }
};

// The cell sizes and bands are the same at every quality (a spark rests as precisely
// at 1 as at 3); the quality buys area. RG16F, 4 bytes a cell:
//   1: fine 192 x 192 x  64 at 4 (768 x 768 x 256),    coarse 128 x 128 x 48 at 16 (2048 x 2048 x 768)    12.6 MB
//   2: fine 256 x 256 x  96 at 4 (1024 x 1024 x 384),  coarse 192 x 192 x 64 at 16 (3072 x 3072 x 1024)   34.6 MB (default)
//   3: fine 384 x 384 x 128 at 4 (1536 x 1536 x 512),  coarse 256 x 256 x 96 at 16 (4096 x 4096 x 1536)  100.7 MB
inline LevelFieldSpec LevelFieldSpecFor(int quality, int level)
{
	static const LevelFieldSpec specs[LEVEL_FIELD_QUALITY_MAX - LEVEL_FIELD_QUALITY_MIN + 1][LEVEL_FIELD_LEVELS] =
	{
		{ { 192, 192,  64,  4, 8 }, { 128, 128, 48, 16, 6 } },
		{ { 256, 256,  96,  4, 8 }, { 192, 192, 64, 16, 6 } },
		{ { 384, 384, 128,  4, 8 }, { 256, 256, 96, 16, 6 } },
	};
	if (quality < LEVEL_FIELD_QUALITY_MIN) quality = LEVEL_FIELD_QUALITY_MIN;
	if (quality > LEVEL_FIELD_QUALITY_MAX) quality = LEVEL_FIELD_QUALITY_MAX;
	if (level < 0) level = 0;
	if (level >= LEVEL_FIELD_LEVELS) level = LEVEL_FIELD_LEVELS - 1;
	return specs[quality - LEVEL_FIELD_QUALITY_MIN][level];
}

// What one frame may hand over: tiles baked (their column parts are staged as texels,
// 1 MiB at 64 tiles), line records (48 bytes each), invalidation boxes.
inline constexpr int LEVEL_FIELD_TILES_PER_FRAME = 64;
inline constexpr int LEVEL_FIELD_LINES_PER_FRAME = 16384;
inline constexpr int LEVEL_FIELD_BOXES_PER_FRAME = 512;

// One line near a tile being baked, std430 with no padding -- must match FieldLine in
// shaders/compute/field_bake.comp. Its WALL PIECES are where exactly one side of it is
// open: a one-sided line's wall from its sector's floor to its ceiling; a two-sided
// line's lower part between the two floors and upper part between the two ceilings.
// Heights at both ends from the planes as they are this frame, linear along the line.
// A part whose top is not above its bottom is no part.
struct LevelFieldLine
{
	float Segment[4];		// x1, y1, x2, y2: map units, Doom axes
	float PartsStart[4];	// at (x1, y1): lower bottom, lower top, upper bottom, upper top
	float PartsEnd[4];		// the same at (x2, y2)
};

// One tile to bake this frame.
struct LevelFieldTile
{
	int32_t Level = 0;
	int32_t TexelTile[3] = { 0, 0, 0 };	// which tile of the volume
	int32_t WorldCell[3] = { 0, 0, 0 };	// the world cell its first texel holds
	int32_t LineFirst = 0;				// its lines: Lines[LineFirst .. LineFirst + LineCount)
	int32_t LineCount = 0;
	// LEVEL_FIELD_TILE_TEXELS floats at Distances + DistanceOffset, x fastest, then y, then
	// z (a 3D image's texel order): the column part, signed -- negative where the cell's
	// centre is solid -- and its magnitude the vertical distance to the column's nearest
	// floor or ceiling, clamped to the band.
	size_t DistanceOffset = 0;
};

// Texel tiles [TexelTileMin, TexelTileMax) of one level, not wrapping, to mark unbaked.
struct LevelFieldBox
{
	int32_t Level = 0;
	int32_t TexelTileMin[3] = { 0, 0, 0 };
	int32_t TexelTileMax[3] = { 0, 0, 0 };
};

// [LEVELFIELD] This frame's level field, decided on the CPU (hw_levelfield.cpp). The
// backend does it in this order: allocate or free, ClearLevel, Invalidate, the header,
// then the tiles (their column parts copied in, then the line pass).
struct LevelFieldFrame
{
	// The field should exist: r_particlecollision on, Vulkan, and a particle that collides
	// with the level was spawned on this map within the linger. False frees it.
	bool Active = false;
	int Quality = LEVEL_FIELD_QUALITY_DEFAULT;
	LevelFieldSpec Levels[LEVEL_FIELD_LEVELS];

	// Every texel of the level unbaked (a new window, a jump, a new map).
	bool ClearLevel[LEVEL_FIELD_LEVELS] = { false, false };

	const LevelFieldBox* Invalidate = nullptr;
	int InvalidateCount = 0;

	const LevelFieldTile* Tiles = nullptr;
	int TileCount = 0;
	const float* Distances = nullptr;
	size_t DistanceCount = 0;
	const LevelFieldLine* Lines = nullptr;
	int LineCount = 0;

	// The header image's texels (gpuparticles.vp):
	//   [0] the fine window: xyz its first world cell (Doom axes), w its cell size in map
	//       units (0 = no level)
	//   [1] the coarse window, the same
	//   [2] x 1 = the field may be used; y 1 = `collide = plane` definitions use it too
	//       (r_particlecollision_test); z the fine band, w the coarse band (map units)
	float Header[LEVEL_FIELD_HEADER_TEXELS][4] = {};
};

// [LEVELFIELD] What the backend did, for the CPU side's NEXT frame. Renderer-internal
// (written by vk_levelfield.cpp, read by hw_levelfield.cpp); never the playsim. Epoch
// rises with every allocation and every failure that emptied the volumes, so the CPU
// side knows its record of what is baked no longer holds.
struct LevelFieldBackendStatus
{
	bool Allocated = false;
	int Quality = 0;
	int RefusedQuality = 0;		// a quality this device refused until collision stops being asked for; 0 = none
	uint64_t Epoch = 0;
};

inline LevelFieldBackendStatus& LevelFieldStatus()
{
	static LevelFieldBackendStatus status;
	return status;
}

struct FrameComputeInput
{
	SmokeVolumeFrame Smoke;

	// [13d] the smoke light grid's per-frame data (r_smoke_light_quality) joins Smoke.
	// [DEBRISPOOL] #9, [SURFACEDAMAGE] #17: their frame data goes here.

	// [LEVELFIELD] #8, filled by LevelField::PrepareFrame (hw_levelfield.cpp).
	LevelFieldFrame LevelField;

	// [EMISSIVEVOLUMES] #15, filled by EmissiveVolumes::PrepareFrame (hw_emissivevolumes.cpp): the list the backend copies into its
	// image (vk_emissivevolumes.cpp). Count 0: nothing to draw, nothing recorded.
	EmissiveVolumeFrame EmissiveVolumes;
};
