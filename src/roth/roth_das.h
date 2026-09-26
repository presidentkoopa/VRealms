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
	uint16_t SkyIndex() const { return mSkyIndex; }
	const std::vector<Colour> &Palette() const { return mPalette; }

	// Decode on demand. `allFrames` also runs the animation delta decoder.
	Image ReadImage(int index, bool allFrames = false) const;
	Mesh ReadMesh(int index) const;

	// Object art is indirected through `textureSource` (handoff 4):
	//   0 -> this pack, index + 4096      2 -> the shared pack, index
	//   1 -> this pack, index + 4096+256  3 -> the shared pack, index + 256
	// Returns true when the shared pack should be used instead of this one.
	static bool ResolveObjectArt(uint8_t textureIndex, uint8_t textureSource,
		int &outIndex);

private:
	const uint8_t *mData = nullptr;
	size_t mSize = 0;
	std::vector<FatEntry> mFat;
	std::vector<Colour> mPalette;
	uint16_t mSkyIndex = 0;
	std::string mError;

	EntryKind Classify(const FatEntry &e) const;
};

// The palette ROTH falls back on when a pack stores none (the shared sprite
// pack does). 768 VGA 6-bit values, expanded the same way embedded ones are.
extern const uint8_t DEFAULT_RAW_PALETTE[768];

} // namespace roth
