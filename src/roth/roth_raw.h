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
#include <unordered_map>
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
	// TEXTURE_MAP_OVERRIDE (RAW.md, sector +0x0c). NOT a height and NOT a light,
	// which is what this field was guessed to be from both sides: it overrides the
	// position and SIZE of the MID_TEXTURE on double-sided faces in this sector
	// that carry TRANSPARENT and TRANSPARENT_FIXED_SIZE. The value is the size; 0
	// means fit-to-size, negative anchors to the floor, positive to the ceiling.
	//
	// Still unused by the loader. tick_modify_sector (opcode 0x03) ramps it, which
	// is now legible: that effect animates a transparent mid-texture size, not a
	// brightness as ROTH.C's comment guessed.
	int8_t   textureMapOverride;
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
	uint16_t unk0C;
	// The handle COMMANDS address this object by, exactly as Sector::commandID is
	// for sectors: resolve_command_objects (raw_commands.c:318) walks every object
	// group and matches this field against the command's key. Objects are a
	// 0x10 stride and this is the field at +0x0e.
	uint16_t commandID;

	bool FixedAngle() const { return (renderType & 0x80) != 0; }
	bool HorizontalFlip() const { return (flags & 0x10) != 0; }
};

// A level's logic, as small programs attached to its geometry. Two kinds of
// record share ONE layout, which is the easiest thing in this format to get
// wrong: a TRIGGER is registered at load and watches for something, while an
// INSTRUCTION is executed in sequence. The word at +4 means different things to
// each -- a trigger's chain START versus the NEXT instruction -- so it is
// resolved here by opcode and consumers never have to know the rule.
//
// See ROTH_COMMANDS.md for the decoded opcode set and what the arguments point
// at. The index space is 1-BASED: ROTH.C's resolve_command_by_index
// (renderer.c:9830) returns base[(index - 1) * 4] and treats 0 as "none".
struct Command
{
	uint8_t  modifier = 0;     // +0x02  state flags; 0x08 = disabled
	uint8_t  opcode = 0;       // +0x03  which instruction or trigger
	uint16_t linkIndex = 0;    // +0x04  raw; use chainStart / nextIndex instead
	uint8_t  fireFlags = 0;    // +0x06  approach mask and fire bits (triggers)
	uint8_t  subFlags = 0;     // +0x07
	uint16_t key = 0;          // +0x08  an ID looked up per-opcode; 0 = "what the player just used"
	uint16_t aux = 0;          // +0x0a  sound effect or auxiliary value
	std::vector<uint16_t> args;// everything from +0x06 on, as 2-byte values

	bool isTrigger = false;
	bool disabled = false;
	uint16_t chainStart = 0;   // triggers: the chain this fires (1-based)
	uint16_t nextIndex = 0;    // instructions: the next one (1-based)

	// What the key resolves to, worked out on load so the runtime never has to.
	// A key is an ID that gets SEARCHED FOR, never an array index --
	// find_geometry_record matches a sector's commandID, and a wall takes two
	// hops: the faceID on an extended mapping record, then the face pointing at
	// that record. See ROTH_COMMANDS.md.
	//
	// Both are -1 / empty when the key names something that is not geometry:
	// flags, items and dialogue are DBASE100 ids, and 0x17/0x38/0x40 take a
	// command index. key == 0 means "whatever the player just used" and is
	// resolved at runtime, not here.
	int sector = -1;                 // key as a sector commandID
	std::vector<int> faces;          // key as a faceID, via the mapping records

	// RAW RECORD ACCESS, by the record offset ROTH.C names.
	//
	// The effect handlers read and WRITE this record at byte precision -- a light
	// switch keeps its signed accumulator in byte[rec+0x0c], a mover its step in
	// byte[rec+0x07] -- and a vector of words cannot express that. These map an
	// offset onto args, which is the word at +0x06 onward, so a handler can be
	// transcribed against the original's offsets without a second layout to keep
	// in step. Out-of-range reads give 0, matching a record too short to hold the
	// field; out-of-range writes are dropped.
	uint16_t Word(int off) const
	{
		if (off < 6 || (off & 1)) return 0;
		const size_t i = (size_t)(off - 6) / 2;
		return i < args.size() ? args[i] : 0;
	}
	uint8_t Byte(int off) const
	{
		if (off < 6) return 0;
		const size_t i = (size_t)(off - 6) / 2;
		if (i >= args.size()) return 0;
		return ((off - 6) & 1) ? (uint8_t)(args[i] >> 8) : (uint8_t)(args[i] & 0xFF);
	}
	void SetByte(int off, uint8_t v)
	{
		if (off < 6) return;
		const size_t i = (size_t)(off - 6) / 2;
		if (i >= args.size()) return;
		if ((off - 6) & 1) args[i] = (uint16_t)((args[i] & 0x00FF) | ((uint16_t)v << 8));
		else               args[i] = (uint16_t)((args[i] & 0xFF00) | v);
	}
	void SetWord(int off, uint16_t v)
	{
		if (off < 6 || (off & 1)) return;
		const size_t i = (size_t)(off - 6) / 2;
		if (i < args.size()) args[i] = v;
	}
};

// The opcodes that are TRIGGERS, from ROTH.C's load-time registration table
// (map_load.c:1089-1097). Everything else is an instruction.
bool IsTriggerOpcode(uint8_t opcode);

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
	// Level logic. commands[i] is the 1-based index i+1.
	std::vector<Command> commands;
	// Where the game starts executing: 1-based indices into commands.
	std::vector<uint16_t> entryPoints;

	// Face record file offset -> face index. Offsets in this format are absolute
	// file positions used as foreign keys, and MOST commands name geometry by a
	// searched id -- but opcode 0x0c names its face by the raw OFFSET, so that
	// lookup has to survive the parse instead of being a local to it.
	std::unordered_map<uint32_t, int> faceByOffset;

	// Applied to every x/y by RecentreWrappedMap (0 unless the map straddles the
	// 16-bit wrap -- RAQUIA2). Anything converting a coordinate read from the
	// original's MEMORY back to ours must add these.
	int wrapShiftX = 0, wrapShiftY = 0;

	std::string error;          // empty when Parse succeeded
	bool ok() const { return error.empty(); }
};

// Parse a whole .RAW file held in memory.
void RecentreWrappedMap(Map &map);
Map ParseRaw(const uint8_t *data, size_t size);

} // namespace roth
