#pragma once
//
// Reader for Realms of the Haunting's .RAW map format.
//
// Deliberately free of engine types: this parses bytes into plain structs and
// nothing else, so it can be exercised without starting the engine. The code
// that turns these into sectors and lines lives in maploader/rothmap.cpp.
//
// Format verified against ROTH.C (the original engine) and cross-checked
// against all 44 retail maps: 16,906 sectors, 82,210 faces, every sector's
// face list forming a closed loop. See ROTH_NATIVE_HANDOFF.md section 4.
//
// Everything is little-endian. Offsets stored in the file are ABSOLUTE byte
// offsets used as foreign keys, not indices -- resolved to indices on load.
//

#include <stdint.h>
#include <vector>
#include <string>

namespace roth
{

// Reserved values in the sector-ID space (ROTH_COMMANDS.md, "the door sentinels
// are three different things"). The original's spawn_door_instance splits on
// these: 0xFFFD goes to a separate six-slot pool, the others to the primary one.
static const uint16_t TRIGGER_DOOR_A   = 0xFFFD; // door, secondary pool; closed at load
static const uint16_t TRIGGER_INVIS    = 0xFFFE; // door-capable; the two-sided wall is not drawn
static const uint16_t TRIGGER_DOOR_B   = 0xFFFF; // door, primary pool; closed at load

// Face texture-mapping flags (the byte at +8 of a mapping record).
enum FaceFlags
{
	FF_TRANSPARENT = 1 << 0, // mid texture drawn in the opening
	FF_FLIP_X      = 1 << 1,
	FF_IMAGE_FIT   = 1 << 2, // exactly one copy across the piece; shifts ignored
	FF_TRANS_UPLO  = 1 << 3, // clips the mid piece to a band
	FF_NO_REFLECT  = 1 << 4, // no effect on texturing
	FF_HALF_PIXEL  = 1 << 5, // one world unit per texel instead of two
	FF_EDGE_MAP    = 1 << 6, // parallax sky above the wall
	FF_PIN_BOTTOM  = 1 << 7, // anchor to the bottom of the piece
};

struct Header
{
	uint16_t verticesOffset, version, sectorsOffset, facesOffset;
	uint16_t faceTextureMapsOffset, mapMetadataOffset, verticesOffsetRepeat;
	uint16_t signature;            // 21079
	uint16_t midPlatformsOffset;   // 0 = none
	uint16_t section7Size, verticesSectionSize, objectsSectionSize;
	uint16_t footerSize, commandSectionSize, sectorCount;
};

struct Sector
{
	int16_t  ceilingHeight, floorHeight;
	uint16_t unk04, ceilingTexture, floorTexture;
	uint8_t  flags;            // bits 2-3 ceiling scale, 4-5 floor scale
	uint8_t  light;
	int8_t   overrideHeight;
	uint8_t  faceCount;
	uint16_t firstFaceOffset;
	uint8_t  ceilShiftX, ceilShiftY, floorShiftX, floorShiftY;
	// The handle commands address this sector by. Commands carry an ID, not an
	// index, and find_geometry_record scans for a match on this field -- see
	// ROTH_COMMANDS.md, "what the key at +0x08 actually names". The door
	// sentinels above are reserved values in the same space.
	uint16_t commandID;
	uint16_t flags2;           // high byte: flat flip bits
	uint16_t platformOffset;   // 0 = none

	// resolved on load
	int firstFaceIndex = -1;
	int platformIndex = -1;

	// Closed at load, so the opening is sealed until something opens it.
	bool IsDoor() const
	{
		return commandID == TRIGGER_DOOR_A || commandID == TRIGGER_DOOR_B;
	}
	// The original's dev_open_nearest_door will only make a door of a wall whose
	// far sector is one of the three sentinels, so TRIGGER_INVIS counts here even
	// though it is not closed at load.
	bool IsDoorCapable() const  { return commandID >= TRIGGER_DOOR_A; }
	bool IsSecondaryDoor() const { return commandID == TRIGGER_DOOR_A; }

	// Which of this door's four faces it swings about. Resolved on load; -1 when
	// the sector is not a door or no hinge was found. See Map::hingeOf.
	int hingeFace = -1;
	// 2^s world units per texel (handoff 5.3)
	int FloorScaleShift() const   { return (flags >> 4) & 3; }
	int CeilingScaleShift() const { return (flags >> 2) & 3; }
};

struct Face
{
	uint16_t vertexOffset1, vertexOffset2, textureMapOffset;
	uint16_t sectorOffset, sisterFaceOffset, collisionFlags;

	// resolved on load
	int vertex1 = -1, vertex2 = -1, textureMap = -1;
	int sector = -1, sister = -1;   // sister < 0 means one-sided
};

struct TextureMap
{
	uint16_t fitWord;          // bit 15 set = extended; low 12 bits = stored extent
	uint16_t midTexture, upperTexture, lowerTexture;
	uint8_t  flags, unk;
	// present only when extended
	uint8_t  shiftX = 0, shiftY = 0;
	uint16_t faceID = 0;
	bool     extended = false;

	// The stored horizontal extent is authoritative and can differ from the
	// wall's measured length (handoff 5.2, 2.4): 12 bits when extended.
	uint16_t StoredExtent() const { return extended ? (fitWord & 0x0FFF) : fitWord; }
};

struct MidPlatform
{
	uint16_t undersideTexture; int16_t undersideZ;
	uint8_t  undersideShiftX, undersideShiftY;
	uint16_t topTexture;       int16_t topZ;
	uint8_t  topShiftX, topShiftY;
	uint8_t  scales;           // bits 2-3 underside, 4-5 top
	uint8_t  pad;
};

struct Metadata
{
	int16_t  startX, startZ, startY, rotation;
	uint16_t moveSpeed, playerHeight, maxClimb, minFit;
	uint16_t shadeLevel;
	int16_t  candleGlow;
	uint16_t lightAmbience, tintFlag, skyTexture, unk;

	// The original doubles these at load (handoff 2.1). Sizing the player from
	// the raw values makes the whole world read as twice its intended size.
	int PlayerHeight() const { return playerHeight * 2; }
	int MaxClimb() const     { return maxClimb * 2; }
	int MinFit() const       { return minFit * 2; }
};

struct Vertex { int16_t x, y; };

struct Object
{
	int16_t  x, y;
	uint8_t  textureIndex, textureSource, rotation, flags, light, renderType;
	int16_t  z;                // ABSOLUTE, not relative to the floor
	uint16_t unk0C, unk0E;

	bool FixedAngle() const { return (renderType & 0x80) != 0; }
	bool HorizontalFlip() const { return (flags & 0x10) != 0; }
};

struct Map
{
	Header header{};
	Metadata metadata{};
	std::vector<Sector> sectors;
	std::vector<Face> faces;
	std::vector<TextureMap> textureMaps;
	std::vector<MidPlatform> platforms;
	std::vector<Vertex> vertices;
	// objects[i] holds the objects belonging to sectors[i]
	std::vector<std::vector<Object>> objects;

	std::string error;          // empty when Parse succeeded
	bool ok() const { return error.empty(); }
};

// Parse a whole .RAW file held in memory.
Map ParseRaw(const uint8_t *data, size_t size);

} // namespace roth
