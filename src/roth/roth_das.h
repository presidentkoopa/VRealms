#pragma once
//
// Reader for Realms of the Haunting's .DAS artwork packs.
//
// Engine-free, like roth_raw.h: this decodes to 8-bit palette-indexed pixels
// and a palette. Turning those into engine textures happens elsewhere.
//
// Verified against the retail packs. See ROTH_NATIVE_HANDOFF.md section 4.
//

#include <stdint.h>
#include <vector>
#include <string>

namespace roth
{

// image_type bits
enum ImageType
{
	IT_ANIMATED     = 1 << 0,
	IT_ZERO_OPAQUE  = 1 << 1, // palette index 0 is a real colour, not a hole
	IT_TRANSLUCENT  = 1 << 2,
	IT_MIRROR       = 1 << 3,
	IT_OBJECT_DATA  = 1 << 7, // a 3D mesh, not a picture
};

// modifier bits
enum ImageModifier
{
	IM_HANG         = 1 << 4, // anchored by its top rather than its bottom
	IM_IMAGE_PACK   = 1 << 6, // several sub-images in one entry
	IM_HALF_SIZE    = 1 << 7,
};

// flags_1 bits on a FAT entry
enum FatFlags
{
	FAT_SKY         = 1 << 1,
	FAT_MONSTER     = 1 << 2, // an indirection, not an image
	FAT_DIRECTIONAL = 1 << 5, // an indirection, not an image
};

enum class EntryKind
{
	Empty,
	Plain,          // a single picture
	Animated,       // first frame raw, the rest delta-compressed
	ImagePack,      // several sub-images
	Object3D,       // vertex/face mesh
	Indirection,    // monster or directional: points elsewhere
};

struct FatEntry
{
	uint32_t offset = 0;
	uint16_t size = 0;
	uint8_t flags1 = 0, flags2 = 0;
	EntryKind kind = EntryKind::Empty;
};

// How an indirection resolves. The original keys this off the whole of flags_1
// rather than off single bits: 0x20 alone goes to the per-map directional
// table, 0x24 spawns a live actor instead (das_assets.c:886-891, where the
// status word is stamped 0xfe or 0xfc accordingly).
enum class IndirectKind
{
	None,
	Directional,    // flags1 == 0x20: art chosen from the viewing angle
	Creature,       // flags1 == 0x24: spawns an actor, art driven by its state
};

// One view's worth of a directional entry.
struct DirectionalFrame
{
	uint16_t entry = 0;     // the FAT index to draw for this view
	bool mirror = false;    // drawn left-right reversed (frame word bit 15)
};

// A view-dependent art entry: which picture is drawn depends on the angle the
// object is seen from. The original picks the frame at renderer.c:5884-5920:
//
//   rec   = dirBlock + u16(dirBlock + flags2 * 2)
//   w     = u16(rec)
//   w & 0x8000 == 0  ->  a fixed frame, through a table held in engine state
//   w & 0x2000       ->  sixteen views, index ((2*rot + 0x110 - view) >> 4) & 0x1e
//   otherwise        ->  eight views,   index ((2*rot + 0x120 - view) >> 5) & 0x0e
//   frame = u16(rec + 2 + index), bit 15 = mirror, low 15 bits = a FAT index
//
// The same 8/16-way rule with the same mirror bit appears in all three of the
// original's frame-picking paths (the resident-block table at renderer.c:5705,
// the creature table at :5844, this one at :5897). It is one rule.
struct Directional
{
	int count = 0;                  // 0 unresolved, 1 fixed, or 8 / 16 views
	DirectionalFrame frames[16];

	bool ok() const { return count > 0; }
	bool ViewDependent() const { return count > 1; }
};

struct Image
{
	int width = 0, height = 0;
	uint8_t modifier = 0, imageType = 0;
	// frames[0] is the still image; later entries exist only for animations.
	std::vector<std::vector<uint8_t>> frames;

	bool ok() const { return width > 0 && height > 0 && !frames.empty(); }
	// Index 0 reads as a hole unless the entry says otherwise.
	bool ZeroIsTransparent() const { return (imageType & IT_ZERO_OPAQUE) == 0; }
};

struct Colour { uint8_t r, g, b; };

// One face of a 3D prop. Realms stores quads as 5 edges and triangles as 4.
struct MeshFace
{
	int vertex[4] = { -1, -1, -1, -1 };
	int count = 0;              // 3 or 4
	uint16_t texture = 0;       // FAT index, or >= 65280 meaning a flat colour
	uint8_t subTexture = 0;
	bool flipV = false;
};

struct Mesh
{
	// Components are (x, up, y): the MIDDLE value is vertical. One mesh unit is
	// one world unit. Do not mirror X (handoff 5.5, 5.1).
	struct Vertex { int16_t x, up, y; };
	std::vector<Vertex> vertices;
	std::vector<MeshFace> faces;
	bool ok() const { return !vertices.empty() && !faces.empty(); }
};

class Pack
{
public:
	// `data` must outlive the Pack; nothing is copied until you ask for it.
	bool Load(const uint8_t *data, size_t size);

	const std::string &Error() const { return mError; }
	int Count() const { return (int)mFat.size(); }
	const FatEntry *Entry(int index) const;
	// THIS IS NOT A SKY INDEX, whatever it was called before. The pack header
	// word at +0x22 is `g_das_unk_0x22` in the original: map_load.c:363 reads
	// it and renderer.c:1518 copies it to 0x89f06, and nothing else touches it.
	//
	// The sky picture is named by the MAP, not by the pack -- the metadata word
	// at +0x18, which roth_raw.h calls `Metadata::skyTexture`. It reaches
	// `g_das_special_fat_index` (map_load.c:214, renderer.c:10306), which is
	// what render_parallax_sky_columns resolves the sky block from
	// (renderer.c:5414) and what the cache pins as its special index
	// (das_assets.c:859).
	//
	// THIS WORD IS THE SKY MARKER: the stored texture INDEX that means "this
	// surface is the sky". The map's own skyTexture says which PICTURE to draw
	// there. Two different quantities, and conflating them cost real time in
	// both directions -- first by reading this as the picture (which threw a
	// finished image away), then by dismissing it as unknown (which painted
	// 6,208 sky surfaces with a flat texture).
	//
	// MEASURED over all 44 retail maps: this word is 0 or 1 and constant per
	// pack, while the maps' sky pictures are 0, 1, 15, 72, 400 and 810. 36 of
	// the 44 maps carry flats using their pack's marker -- 6,208 of them, from
	// 2 in OPTEMP1 to 594 ceilings in TOWER1, whose sky picture (72) is an
	// ANIMATED entry, i.e. moving cloud.
	uint16_t SkyMarkerIndex() const { return mUnknown0x22; }
	const std::vector<Colour> &Palette() const { return mPalette; }

	// Decode on demand. `allFrames` also runs the animation delta decoder.
	Image ReadImage(int index, bool allFrames = false) const;
	Mesh ReadMesh(int index) const;

	// Which of the two indirections this entry is, if either.
	IndirectKind Indirect(int index) const;

	// Resolve a directional entry to the pictures for each view. Returns an
	// empty Directional when the entry is not directional, when the pack has
	// no directional block, or when the record runs outside it.
	Directional ReadDirectional(int index) const;

	// True when the pack carries a directional block at all.
	bool HasDirectional() const { return mDirSize > 0; }

	// Object art is indirected through `textureSource` (handoff 4):
	//   0 -> this pack, index + 4096      2 -> the shared pack, index
	//   1 -> this pack, index + 4096+256  3 -> the shared pack, index + 256
	// Returns true when the shared pack should be used instead of this one.
	static bool ResolveObjectArt(uint8_t textureIndex, uint8_t textureSource,
		int &outIndex);

	// A map has two packs open at once and the DAS ids in its data are numbered
	// across BOTH: ids below this are the map's own pack, ids at or above it
	// are the shared pack, numbered from zero again.
	//
	// select_das_fat_entry (renderer.c:730-733) is the whole rule: it holds the
	// index pre-doubled, so its `>= 0x2400` test and `-= 0x2400` are this
	// boundary. The DAS cache picks the file the same way (das_assets.c:930).
	//
	// ResolveObjectArt answers the same question for an OBJECT's art, where the
	// map data carries a pack selector beside the index; this one is for ids
	// that arrive bare, as a directional entry's frames do.
	static const int SHARED_ID_BASE = 0x1200;
	static bool ResolveDasId(int id, int &outIndex);

private:
	const uint8_t *mData = nullptr;
	size_t mSize = 0;
	std::vector<FatEntry> mFat;
	std::vector<Colour> mPalette;
	uint16_t mUnknown0x22 = 0;   // header +0x22; see SkyMarkerIndex()
	// The directional-object block: header +0x1c is its file offset (0 means
	// the pack has none) and +0x1a its byte size. The original reads it into
	// its own allocation at map load (map_load.c:367, :385-390); here it stays
	// where it is in the mapped file.
	uint32_t mDirOffset = 0;
	uint16_t mDirSize = 0;
	std::string mError;

	EntryKind Classify(const FatEntry &e) const;
	// One animation frame's edit stream, applied in place to the previous
	// frame. `p` is a file offset. See the comment on the definition.
	void ApplyFrameDelta(std::vector<uint8_t> &frame, size_t p) const;
};

// The palette ROTH falls back on when a pack stores none (the shared sprite
// pack does). 768 VGA 6-bit values, expanded the same way embedded ones are.
extern const uint8_t DEFAULT_RAW_PALETTE[768];

} // namespace roth
