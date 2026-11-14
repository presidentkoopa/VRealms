#pragma once
//
// THE CLOSE-UP VOXELIZER: a voxel model of an inventory item, built at load
// from the item's own close-up film in the player's install.
//
// Engine-free (the C++ standard library and roth_gdv only), like everything
// else here that reads the retail data: tools/voxelize_test builds it and runs
// it over every item of an install. Nothing it makes is written anywhere by
// this code, and nothing derived from the retail art ships: the engine runs it
// on the player's own DBASE300.DAT.
//
// THE SOURCE. 280 of the 281 DBASE100 inventory records name a close-up film
// (record +0x08 = DBASE300 offset / 8; the GDV starts 4 bytes after offset * 8,
// behind its size dword). For the objects, those films are TURNTABLES of the
// item as it was pre-rendered in 1996: the item turns about a vertical axis in
// front of a fixed camera that usually looks down at it, on a black ground,
// with the lighting baked in, in the GDV's own 256-colour palette. Some films
// are not turntables (people talking, places, letters being unfolded): the
// geometry estimate says so (VoxGeometry::turntable) and no model is made.
//
// THE METHOD (VOXELIZER.md has the measurements and the limits).
//   1. Masks. The ground is the palette index (or the two dithered indices)
//      that fills the picture's border: each index holding at least 10% of
//      the border pixels, all of them near black. In the item films that is
//      index 0 alone, except the Haunch's, whose ground is a checkerboard of
//      index 1 (black) and 142 (dark green). The item's own black pixels use
//      OTHER black entries (the renderer dithered dark parts with indices 1,
//      47, 63, ... whose colour is also 0,0,0), so the mask is "index not in
//      the ground", never "colour != black".
//   2. The turntable. Unknowns: frames per revolution (the film's frame count,
//      or one less when the last frame repeats the first, or a half or double
//      turn), the turning direction, the camera's elevation, the screen column
//      of the axis, the camera's roll, and the perspective (orthographic
//      first; a finite camera distance is tried). Every candidate is scored by
//      SILHOUETTE CONSISTENCY: carve the visual hull with it, project the hull
//      back into every frame, and take the mean IoU with the masks. A wrong
//      camera carves too much and the IoU drops. Search coarse to fine.
//      The direction and the elevation's sign are tied for silhouettes (the
//      mirror object turning the other way, seen from below, casts the same
//      silhouettes), so the elevation is searched from -8 degrees up (the
//      films look level or down at their items; the only films that fitted
//      better from below were ones that are not turntables at all), and a
//      photo-consistency score breaks the remaining ties (direction for a
//      level camera, period for a round item, elevation for a round item).
//   3. Carving at the film's own resolution (one voxel = one film pixel at
//      the axis by default), then colour per surface voxel from the frames
//      that see it (z-buffered), weighted by how squarely they see it.
//   4. KVX. The engine loads Build KVX voxels (src/common/models/voxels.cpp,
//      R_LoadKVX) and draws them as models (models_voxel.cpp). VoxBuildKvx
//      writes the KVX bytes in memory: one mip, the slabs of the surface
//      voxels with their exposed faces, the film's own 6-bit palette.
//
#include <stdint.h>
#include <string>
#include <vector>

namespace roth
{

// The elevation the retail films settle on when their silhouettes pin it
// (the median of the 62 pinned within 6 degrees; VOXELIZER.md), used for the
// films whose silhouettes do not (round items).
static const double VOX_TYPICAL_ELEVATION_DEG = 20.0;

// One close-up film, decoded: every frame as palette indices, full size.
struct VoxFilm
{
	int width = 0, height = 0, frames = 0;
	std::vector<uint8_t> pixels;        // frames * height * width
	uint8_t palette[768] = {};          // 8-bit RGB, the DAC after the first frame
	bool paletteChanged = false;        // the DAC differed on a later frame (none in retail)
	int ground = -1;                    // the background's main palette index; -1 = not an object on black
	bool groundSet[256] = {};           // every index of the ground (a dithered ground has two)
	double groundBorderShare = 0;       // share of border pixels that are ground indices
	const uint8_t *Frame(int k) const { return pixels.data() + (size_t)k * width * height; }
};

// Decode a close-up film: `gdv` points at the GDV magic (DBASE300 offset * 8
// + 4). Finds the ground index too.
bool VoxDecodeFilm(const uint8_t *gdv, size_t size, VoxFilm &out, std::string *error = nullptr);

// The film's GDV inside DBASE300.DAT for a DBASE100 close-up value (record
// +0x08): the bytes after the size dword. False when out of range.
bool VoxCloseupBytes(const uint8_t *dbase300, size_t size, uint32_t closeup, const uint8_t *&gdv, size_t &gdvSize);

// Foreground masks, one byte a pixel (1 = item), frames * height * width.
// Specks (8-connected parts of `minSpeck` pixels or fewer) are dropped.
void VoxBuildMasks(const VoxFilm &film, std::vector<uint8_t> &masks, int minSpeck = 2);

// The turntable camera. Object frame: X right and Z toward the camera at
// frame 0's pose, Y up, units = film pixels at the axis; the axis is X = Z = 0.
struct VoxCamera
{
	double period = 20;         // frames per full turn
	int direction = 1;          // +1 or -1
	double elevation = 0;       // radians, > 0 = looking down
	double axisX = 0;           // screen column of the axis (pixel units, 0 = left edge of the picture)
	double axisY = 0;           // screen row where Y = 0 lands
	double roll = 0;            // radians
	double invDistance = 0;     // 1 / camera distance (film pixels); 0 = orthographic
	int firstFrame = 0;         // frames before this are not part of the turn
	int lastFrame = -1;         // last frame of the turn (inclusive); -1 = film end
	// Per-frame corrections to the uniform turn, radians (empty = none): the
	// films' keyframed turns need not step evenly.
	std::vector<double> angleOffset;

	double Angle(int k) const;
	// Screen position (u right, v down, pixel units) and depth toward the
	// camera of an object-frame point at frame k.
	void Project(int k, double x, double y, double z, double &u, double &v, double &depth) const;
	// The direction toward the camera at frame k, in the object frame.
	void ViewDir(int k, double d[3]) const;
};

struct VoxGeometry
{
	bool turntable = false;     // the film is a rigid turntable (a model can be made)
	VoxCamera camera;
	double meanIoU = 0;         // silhouette consistency at the estimate (search grid)
	double minFrameIoU = 0;     // its worst frame
	double elevationLow = 0, elevationHigh = 0;    // radians: where the score stays within 0.01 of the best
	bool elevationOpen = false; // the silhouettes did not pin it: VOX_TYPICAL_ELEVATION_DEG was used
	double photoError = -1;     // mean RGB spread of a surface point across the frames that see it
	double runnerUpIoU = 0;     // the best score of a different period/direction
	std::string runnerUp;       // what it was
	std::string note;           // why it is not a turntable, or what is uncertain
	int evaluations = 0;
};

struct VoxEstimateOptions
{
	int coarseCells = 44;       // longest side of the search grid, in voxels
	int fineCells = 88;         // the refinement's
	double minIoU = 0.80;       // below (mean over the turn), or
	double minFrameIoU = 0.60;  // below in any one frame: not a rigid turntable
	bool tryPerspective = true;
	bool tryRoll = true;
};

VoxGeometry VoxEstimateTurntable(const VoxFilm &film, const std::vector<uint8_t> &masks,
	const VoxEstimateOptions &opt = VoxEstimateOptions());

// The voxel model. Index (x, y, z) = x + sx * (y + sy * z): x right, y UP,
// z toward the camera at frame 0 (the object frame above, in voxels).
struct VoxModel
{
	int sx = 0, sy = 0, sz = 0;
	double voxelPx = 1;         // edge of a voxel in film pixels (at the axis)
	double originX = 0, originY = 0, originZ = 0;   // object-frame position of voxel (0,0,0)'s low corner
	std::vector<uint8_t> solid;     // 1 = filled
	std::vector<uint8_t> colour;    // palette index of a surface voxel
	uint8_t palette[768] = {};      // 8-bit RGB
	int photoCarved = 0;            // voxels the photo-consistency pass removed
	size_t Index(int x, int y, int z) const { return (size_t)x + (size_t)sx * ((size_t)y + (size_t)sy * (size_t)z); }
	bool Solid(int x, int y, int z) const
	{
		return x >= 0 && y >= 0 && z >= 0 && x < sx && y < sy && z < sz && solid[Index(x, y, z)];
	}
	bool Surface(int x, int y, int z) const;
	int Count() const;
	int SurfaceCount() const;
};

enum class VoxColourMode
{
	Front,      // the frame that sees the voxel most squarely: the film's look, its lighting from the camera
	Median,     // of the frames that see it well, the one of median brightness: drops highlights and deep shade
	Mean,       // their mean colour, matched back to the film palette: flattest light, softest detail
};

struct VoxCarveOptions
{
	double voxelPx = 1.0;       // voxel edge in film pixels
	int missTolerance = 0;      // a voxel survives being outside this many frames' masks
	VoxColourMode colourMode = VoxColourMode::Mean;
	bool deDither = true;       // read the film through a 3 x 3 filter over the item's pixels
	bool ownPalette = true;     // 256 colours cut from the model's own (else the film's palette)
	int maxSide = 255;          // KVX limit (bytes); the voxel grows when the item needs more
	// Photo-consistency carving after the silhouettes (see PhotoCarve).
	bool photoCarve = true;
	int carveRounds = 40;
	int carveDepth = 24;        // voxels below the hull's surface it may reach
	int carveMinViews = 3;      // a voxel seen by fewer frames is not judged
	double carveHueBound = 22;  // RGB spread (0..255) with each sample's brightness evened out
	double carveRawBound = 48;  // RGB spread of the plain samples
	double sweepGain = 0.7;     // the depth sweep moves the surface in where the spread drops below this share
};

// Carve the visual hull and colour it.
bool VoxCarve(const VoxFilm &film, const std::vector<uint8_t> &masks, const VoxCamera &cam,
	const VoxCarveOptions &opt, VoxModel &out);

// Project the model into every frame of the turn: IoU with the mask, per
// frame (frames outside the turn get -1).
std::vector<double> VoxReprojectIoU(const VoxModel &m, const VoxCamera &cam, const std::vector<uint8_t> &masks,
	int width, int height, int frames);

// Mean absolute RGB difference between the model drawn with the film's camera
// and the film's frame, over the pixels both cover (0..255).
double VoxPhotometricError(const VoxModel &m, const VoxCamera &cam, const VoxFilm &film,
	const std::vector<uint8_t> &masks);

// Draw the model (z-buffered splats) from a camera at `elevation`, turned to
// `angle` (radians) about the axis, `scale` output pixels per voxel. RGB,
// `bg` behind it. For the test tool's pictures.
void VoxRender(const VoxModel &m, double angle, double elevation, double scale, int w, int h,
	std::vector<uint8_t> &rgb, const uint8_t bg[3]);

// Draw the model with the film's own camera at frame k, the film's size.
void VoxRenderFrame(const VoxModel &m, const VoxCamera &cam, int k, int w, int h, std::vector<uint8_t> &rgb, const uint8_t bg[3]);

// The model as a Build KVX file in memory (one mip), the bytes R_LoadKVX
// reads. KVX axes: x = model x, y = model z (toward frame 0's camera),
// z = DOWN (sy - 1 - model y); the pivot is the turntable axis at the base.
// The palette is the film's, 6-bit. Empty on failure (a side over 255, or an
// x column's slabs over 32767 bytes, which the KVX offsets cannot hold).
std::vector<uint8_t> VoxBuildKvx(const VoxModel &m, std::string *error = nullptr);

// THE FITTED-NUMBERS TABLE. The turntable search costs seconds per film, too
// slow for load, and the engine writes nothing to disk but savegames. So the
// numbers the search fits (no pixels, no art) are kept per film in
// roth_voxelize_table.h, keyed by the CRC-32 and size of the film's GDV bytes;
// carving still happens at load, from the player's own film. A film not in
// the table (another release, a damaged file) is searched.
struct VoxFittedFilm
{
	uint32_t crc, size;
	float period;
	int8_t direction;
	uint8_t elevationOpen;
	int16_t firstFrame, lastFrame;
	float elevationDeg, axisX, axisY, rollDeg;
	float distance;             // 0 = orthographic
};
uint32_t VoxFilmCrc(const uint8_t *gdv, size_t size);
// Null when the film is not in the table.
const VoxFittedFilm *VoxFindFitted(uint32_t crc, uint32_t size);
VoxCamera VoxCameraFromFitted(const VoxFittedFilm &f);
VoxFittedFilm VoxFittedFromCamera(uint32_t crc, uint32_t size, const VoxCamera &c, bool elevationOpen);
// How many films the table holds.
int VoxFittedCount();

// Everything above for one film: the table's numbers when the film is in it
// (useTable), else the search.
struct VoxResult
{
	bool ok = false;
	bool fromTable = false;     // the camera came from roth_voxelize_table.h
	VoxGeometry geometry;
	VoxModel model;
	std::string error;
};
VoxResult VoxelizeCloseup(const uint8_t *gdv, size_t size, const VoxEstimateOptions &est = VoxEstimateOptions(),
	const VoxCarveOptions &carve = VoxCarveOptions(), bool useTable = true);

} // namespace roth
