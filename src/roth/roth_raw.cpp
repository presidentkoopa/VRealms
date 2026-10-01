//
// Realms of the Haunting .RAW map reader.
//
// Ported from a Python implementation validated against all 44 retail maps.
// Deliberately engine-free; see roth_raw.h.
//

#include "roth_raw.h"

#include <algorithm>

#include <string.h>
#include <unordered_map>

namespace roth
{

namespace
{

// A bounds-checked little-endian cursor. Every read is range-tested, so a
// truncated or hostile file yields an error rather than reading out of bounds.
class Reader
{
public:
	Reader(const uint8_t *data, size_t size) : mData(data), mSize(size) {}

	bool Bad() const  { return mBad; }
	size_t Tell() const { return mPos; }
	void Seek(size_t p) { if (p > mSize) mBad = true; else mPos = p; }
	bool AtEnd() const { return mPos >= mSize; }

	uint8_t U8()
	{
		if (mPos + 1 > mSize) { mBad = true; return 0; }
		return mData[mPos++];
	}
	int8_t I8() { return (int8_t)U8(); }

	uint16_t U16()
	{
		if (mPos + 2 > mSize) { mBad = true; return 0; }
		uint16_t v = (uint16_t)(mData[mPos] | (mData[mPos + 1] << 8));
		mPos += 2;
		return v;
	}
	int16_t I16() { return (int16_t)U16(); }

	uint32_t U32()
	{
		uint32_t lo = U16(), hi = U16();
		return lo | (hi << 16);
	}

	void Skip(size_t n) { Seek(mPos + n); }

private:
	const uint8_t *mData;
	size_t mSize;
	size_t mPos = 0;
	bool mBad = false;
};

const size_t FACE_RECORD_SIZE = 0x0C;

} // namespace

// From ROTH.C's load-time trigger-registration table (map_load.c:1089-1097).
// Everything not listed here is an instruction, executed rather than watched for.
bool IsTriggerOpcode(uint8_t opcode)
{
	switch (opcode)
	{
	case 0x08:   // the player clicks or uses an object
	case 0x13:   // enters or leaves water or lava; fires on both edges
	case 0x18:   // enters a sector, channel A
	case 0x19:   // uses a wall
	case 0x1A:   // bumps into a wall
	case 0x1B:   // touches or activates an object
	case 0x25:   // inert: does nothing in either table
	case 0x30:   // marks matching objects; what reads the mark is unresolved
	case 0x31:   // uses a wall, direction-sensitive
	case 0x32:   // enters a sector, channel B
	case 0x3D:   // a timer, started at level load
		return true;
	default:
		return false;
	}
}


//==========================================================================
//
// WRAP RE-CENTRING. Realms coordinates are 16-bit and the original's maths
// wraps, so a map may legitimately straddle +-32768: RAQUIA2's content runs
// over y = [20480, 32767] and [-32768, -26112], one continuous area in the
// original. Read as plain int16 it splits in two and its boundary walls become
// ~64,000 units long. Found by the oracle survey, 2026-10-01.
//
// Per axis: find the largest empty gap on the 16-bit circle. If it is NOT the
// one that already contains the wrap point, shift every coordinate so the gap
// sits at the wrap, by a multiple of 2048 so world-anchored flats keep their
// alignment (2048 is a multiple of every texture period: <= 256 texels at
// <= 8 units). Maps that do not wrap are untouched.
//
//==========================================================================

static int WrapShiftFor(std::vector<int> vals)
{
	if (vals.size() < 2) return 0;
	std::sort(vals.begin(), vals.end());
	vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
	// gaps between neighbours, plus the gap that crosses +-32768
	int bestGap = (vals.front() + 65536) - vals.back();   // the wrap gap
	int bestLo = vals.back();
	bool wrapIsBest = true;
	for (size_t i = 1; i < vals.size(); i++)
	{
		const int g = vals[i] - vals[i - 1];
		if (g > bestGap) { bestGap = g; bestLo = vals[i - 1]; wrapIsBest = false; }
	}
	if (wrapIsBest) return 0;
	// content starts just above the gap and runs (65536 - gap) round the circle;
	// move its centre to 0
	const int start = bestLo + bestGap;
	const int span = 65536 - bestGap;
	int centre = start + span / 2;
	int shift = -centre;
	shift = (int)((shift >= 0 ? shift + 1024 : shift - 1024) / 2048) * 2048;
	return shift;
}

static int16_t Wrap16(int v, int shift)
{
	return (int16_t)(uint16_t)((v + shift) & 0xFFFF);
}

void RecentreWrappedMap(Map &map)
{
	std::vector<int> xs, ys;
	for (const Vertex &v : map.vertices) { xs.push_back(v.x); ys.push_back(v.y); }
	const int sx = WrapShiftFor(xs), sy = WrapShiftFor(ys);
	map.wrapShiftX = sx;
	map.wrapShiftY = sy;
	if (sx == 0 && sy == 0) return;
	for (Vertex &v : map.vertices) { v.x = Wrap16(v.x, sx); v.y = Wrap16(v.y, sy); }
	for (auto &group : map.objects)
		for (Object &o : group) { o.x = Wrap16(o.x, sx); o.y = Wrap16(o.y, sy); }
	map.metadata.startX = Wrap16(map.metadata.startX, sx);
	map.metadata.startY = Wrap16(map.metadata.startY, sy);
}

Map ParseRaw(const uint8_t *data, size_t size)
{
	Map map;
	Reader r(data, size);

	// ---- header -----------------------------------------------------------
	Header &h = map.header;
	h.verticesOffset = r.U16();
	h.version = r.U16();
	h.sectorsOffset = r.U16();
	h.facesOffset = r.U16();
	h.faceTextureMapsOffset = r.U16();
	h.mapMetadataOffset = r.U16();
	h.verticesOffsetRepeat = r.U16();
	h.signature = r.U16();
	h.midPlatformsOffset = r.U16();
	h.section7Size = r.U16();
	h.verticesSectionSize = r.U16();
	h.objectsSectionSize = r.U16();
	h.footerSize = r.U16();
	h.commandSectionSize = r.U16();
	h.sectorCount = r.U16();

	if (r.Bad())
	{
		map.error = "truncated header";
		return map;
	}
	if (h.signature != 21079)
	{
		map.error = "not a ROTH map (bad signature)";
		return map;
	}

	// ---- sectors ----------------------------------------------------------
	map.sectors.reserve(h.sectorCount);
	for (int i = 0; i < h.sectorCount; i++)
	{
		Sector s;
		s.ceilingHeight = r.I16();
		s.floorHeight = r.I16();
		s.unk04 = r.U16();
		s.ceilingTexture = r.U16();
		s.floorTexture = r.U16();
		s.flags = r.U8();
		s.light = r.U8();
		s.textureMapOverride = r.I8();
		s.faceCount = r.U8();
		s.firstFaceOffset = r.U16();
		s.ceilShiftX = r.U8();
		s.ceilShiftY = r.U8();
		s.floorShiftX = r.U8();
		s.floorShiftY = r.U8();
		s.commandID = r.U16();
		s.flags2 = r.U16();
		s.platformOffset = r.U16();
		map.sectors.push_back(s);
	}

	// ---- faces ------------------------------------------------------------
	// Offsets in this format are absolute file positions used as foreign keys,
	// so record where each record began and resolve to indices afterwards.
	std::unordered_map<uint32_t, int> &faceByOffset = map.faceByOffset;
	uint16_t faceCount = r.U16();
	map.faces.reserve(faceCount);
	for (int i = 0; i < faceCount; i++)
	{
		faceByOffset[(uint32_t)r.Tell()] = i;
		Face f;
		f.vertexOffset1 = r.U16();
		f.vertexOffset2 = r.U16();
		f.textureMapOffset = r.U16();
		f.sectorOffset = r.U16();
		f.sisterFaceOffset = r.U16();
		f.collisionFlags = r.U16();
		map.faces.push_back(f);
	}

	// ---- face texture mappings -------------------------------------------
	std::unordered_map<uint32_t, int> mapByOffset;
	uint16_t mapCount = r.U16();
	map.textureMaps.reserve(mapCount);
	for (int i = 0; i < mapCount; i++)
	{
		mapByOffset[(uint32_t)r.Tell()] = i;
		TextureMap t;
		t.fitWord = r.U16();
		t.midTexture = r.U16();
		t.upperTexture = r.U16();
		t.lowerTexture = r.U16();
		t.flags = r.U8();
		t.unk = r.U8();
		// The top bit of the first word means four more bytes follow. Records
		// are therefore variable-length; nothing may assume a fixed stride.
		if (t.fitWord & 0x8000)
		{
			t.extended = true;
			t.shiftX = r.U8();
			t.shiftY = r.U8();
			t.faceID = r.U16();
		}
		map.textureMaps.push_back(t);
	}

	// ---- mid-platforms (optional) ----------------------------------------
	std::unordered_map<uint32_t, int> platformByOffset;
	if (h.midPlatformsOffset != 0)
	{
		uint16_t count = r.U16();
		map.platforms.reserve(count);
		for (int i = 0; i < count; i++)
		{
			platformByOffset[(uint32_t)r.Tell()] = i;
			MidPlatform p;
			p.undersideTexture = r.U16();
			p.undersideZ = r.I16();
			p.undersideShiftX = r.U8();
			p.undersideShiftY = r.U8();
			p.topTexture = r.U16();
			p.topZ = r.I16();
			p.topShiftX = r.U8();
			p.topShiftY = r.U8();
			p.scales = r.U8();
			p.pad = r.U8();
			map.platforms.push_back(p);
		}
	}

	// ---- metadata ---------------------------------------------------------
	Metadata &m = map.metadata;
	m.startX = r.I16();
	m.startZ = r.I16();
	m.startY = r.I16();
	m.rotation = r.I16();
	m.moveSpeed = r.U16();
	m.playerHeight = r.U16();
	m.maxClimb = r.U16();
	m.minFit = r.U16();
	m.shadeLevel = r.U16();
	m.candleGlow = r.I16();
	m.lightAmbience = r.U16();
	m.tintFlag = r.U16();
	m.skyTexture = r.U16();
	m.unk = r.U16();

	// ---- vertices ---------------------------------------------------------
	r.Seek(h.verticesOffset);
	r.U16();                       // section size
	r.U16();                       // header size
	r.U16();                       // blank
	uint16_t vertexCount = r.U16();

	// Vertex offsets in faces are relative to the start of this section.
	std::unordered_map<uint32_t, int> vertexByOffset;
	map.vertices.reserve(vertexCount);
	for (int i = 0; i < vertexCount; i++)
	{
		vertexByOffset[(uint32_t)(r.Tell() - h.verticesOffset)] = i;
		r.Skip(8);                 // four unused words
		Vertex v;
		v.x = r.I16();
		v.y = r.I16();
		map.vertices.push_back(v);
	}

	if (r.Bad())
	{
		map.error = "truncated before objects";
		return map;
	}

	// ---- level logic ------------------------------------------------------
	// The command section follows the vertices. Two record kinds share one
	// layout; see roth_raw.h and ROTH_COMMANDS.md.
	//
	// Layout, in order: a 8-byte header (a 2-char signature, a word, the
	// commands' offset, the record count), then FIFTEEN category slots of
	// (offset, count), then one 1-based entry offset per record, then the
	// records themselves. Offsets are relative to the start of this section.
	//
	// The index space is 1-BASED because that is what the original uses:
	// resolve_command_by_index (renderer.c:9830) returns base[(index-1)*4] and
	// treats index 0 as "none".
	{
		const size_t commandsBase = (size_t)h.verticesOffset + h.verticesSectionSize;
		r.Skip(2);                             // signature
		r.U16();                               // unknown
		r.U16();                               // commands offset
		const uint16_t commandCount = r.U16();
		for (int i = 0; i < 15; i++) { r.U16(); r.U16(); }   // category slots

		std::vector<uint16_t> entryOffsets;
		entryOffsets.reserve(commandCount);
		for (int i = 0; i < commandCount; i++) entryOffsets.push_back(r.U16());

		std::unordered_map<uint32_t, uint16_t> commandByOffset;
		map.commands.reserve(commandCount);
		for (int i = 0; i < commandCount && !r.Bad(); i++)
		{
			commandByOffset[(uint32_t)(r.Tell() - commandsBase)] = (uint16_t)(i + 1);

			const size_t recStart = r.Tell();
			const uint16_t size = r.U16();
			Command c;
			c.modifier  = r.U8();
			c.opcode    = r.U8();
			c.linkIndex = r.U16();

			// Arguments fill whatever is left of the record. The named fields
			// below are the first three of them, kept separately because every
			// opcode uses them the same way.
			const int argCount = size >= 6 ? (int)((size - 6) / 2) : 0;
			c.args.reserve(argCount);
			for (int a = 0; a < argCount; a++) c.args.push_back(r.U16());
			if (argCount > 0) { c.fireFlags = (uint8_t)(c.args[0] & 0xFF);
			                    c.subFlags  = (uint8_t)(c.args[0] >> 8); }
			if (argCount > 1) c.key = c.args[1];
			if (argCount > 2) c.aux = c.args[2];

			c.isTrigger = IsTriggerOpcode(c.opcode);
			c.disabled  = (c.modifier & 0x08) != 0;
			// The SAME word at +4 means opposite things to the two record
			// kinds. Reading it as "next" for a trigger wires every trigger in
			// the game to the wrong place, so it is resolved once, here.
			if (c.isTrigger) c.chainStart = c.linkIndex;
			else             c.nextIndex  = c.linkIndex;

			map.commands.push_back(c);
			if (size >= 6) r.Seek(recStart + size);   // records are self-sizing
		}

		for (uint16_t off : entryOffsets)
		{
			if (off == 0) continue;                    // 0 is "no entry"
			auto it = commandByOffset.find(off);
			if (it != commandByOffset.end()) map.entryPoints.push_back(it->second);
		}
	}

	// ---- objects ----------------------------------------------------------
	// Laid out as a table of per-sector offsets relative to the start of the
	// section, each pointing at a small run of object records.
	map.objects.resize(map.sectors.size());
	{
		size_t sectionStart = (size_t)h.verticesOffset + h.verticesSectionSize
			+ h.commandSectionSize + h.section7Size;
		r.Seek(sectionStart);
		r.U16();                   // section size
		size_t cursor = r.Tell();

		for (size_t i = 0; i < map.sectors.size(); i++)
		{
			r.Seek(cursor);
			cursor += 2;
			uint16_t rel = r.U16();
			if (rel == 0 || r.Bad())
				continue;

			r.Seek(sectionStart + rel);
			uint8_t count = r.U8();
			r.U8();                // repeat of the count
			for (int j = 0; j < count && !r.Bad(); j++)
			{
				Object o;
				o.x = r.I16();
				o.y = r.I16();
				o.textureIndex = r.U8();
				o.textureSource = r.U8();
				o.rotation = r.U8();
				o.flags = r.U8();
				o.light = r.U8();
				o.renderType = r.U8();
				o.z = r.I16();
				o.unk0C = r.U16();
				o.commandID = r.U16();
				map.objects[i].push_back(o);
			}
		}
	}

	// ---- resolve offsets into indices -------------------------------------
	for (size_t i = 0; i < map.sectors.size(); i++)
	{
		Sector &s = map.sectors[i];

		auto itFace = faceByOffset.find(s.firstFaceOffset);
		if (itFace == faceByOffset.end())
		{
			map.error = "sector points at an unknown face";
			return map;
		}
		s.firstFaceIndex = itFace->second;

		// Faces don't name their sector directly; a sector claims a run of them.
		for (int j = 0; j < s.faceCount; j++)
		{
			auto it = faceByOffset.find(s.firstFaceOffset + FACE_RECORD_SIZE * j);
			if (it != faceByOffset.end())
				map.faces[it->second].sector = (int)i;
		}

		if (s.platformOffset != 0)
		{
			auto it = platformByOffset.find(s.platformOffset);
			if (it != platformByOffset.end())
				s.platformIndex = it->second;
		}
	}

	for (Face &f : map.faces)
	{
		auto v1 = vertexByOffset.find(f.vertexOffset1);
		auto v2 = vertexByOffset.find(f.vertexOffset2);
		auto tm = mapByOffset.find(f.textureMapOffset);
		if (v1 == vertexByOffset.end() || v2 == vertexByOffset.end()
			|| tm == mapByOffset.end())
		{
			map.error = "face points at unknown geometry";
			return map;
		}
		f.vertex1 = v1->second;
		f.vertex2 = v2->second;
		f.textureMap = tm->second;

		// 0xFFFF means one-sided. 0x0000 appears in some maps and is also not a
		// real sister, so treat both as "no neighbour".
		if (f.sisterFaceOffset != 0xFFFF && f.sisterFaceOffset != 0x0000)
		{
			auto s = faceByOffset.find(f.sisterFaceOffset);
			if (s != faceByOffset.end())
				f.sister = s->second;
		}
	}

	// THE HINGE, resolved AFTER the faces above -- it reads face.textureMap, so
	// it cannot run in the sector loop where firstFaceIndex is set. It did, once,
	// and found nothing at all while the same rule matched 141 out of 141 offline.
	//
	// A Realms door is a four-walled slab that swings about one corner, and the
	// original finds that corner by walking the slab's four walls for the one
	// whose mapping record is EXTENDED and whose faceID is a door sentinel
	// (setup_door_swing_geometry, doors.c:496-503: it tests fs:[edge]&0x8000,
	// the record's extended bit, and fs:[edge+0xc] >= 0xfffd, its faceID).
	//
	// VERIFIED across all 44 retail maps: 141 door sectors, every one with
	// exactly four faces, every one with exactly one hinge. No exceptions.
	for (auto &s : map.sectors)
	{
		// IsDoorCapable, not IsDoor: 0xFFFE is the second panel of a double door
		// (resolve_door_neighbor_sector accepts >= 0xFFFE, doors.c:320), so it needs
		// a hinge too or nothing can be built for it.
		if (!s.IsDoorCapable() || s.faceCount != 4 || s.firstFaceIndex < 0) continue;
		for (int j = 0; j < 4; j++)
		{
			const int fi = s.firstFaceIndex + j;
			if (fi < 0 || fi >= (int)map.faces.size()) continue;
			const int tmi = map.faces[fi].textureMap;
			if (tmi < 0 || tmi >= (int)map.textureMaps.size()) continue;
			const TextureMap &tm = map.textureMaps[tmi];
			if (tm.extended && tm.faceID >= TRIGGER_DOOR_A) { s.hingeFace = fi; break; }
		}
	}

	// ---- resolve what each command's key points at -------------------------
	// A key is an ID the original SEARCHES for, never an array index.
	//
	//   find_geometry_record (raw_commands.c:50) walks the sectors and matches
	//   the field we call commandID.
	//   A wall takes TWO HOPS: collect_raw_state_matches (renderer.c:11222)
	//   gathers the extended mapping records whose faceID equals the key, then
	//   find_face_record (raw_commands.c:64) finds the face pointing at each.
	//
	// Several walls can share one faceID, which is how a single command repaints
	// or moves a whole group of them.
	//
	// ONE MORE RULE, from the original's own load pass (map_load.c:1158-1172):
	// when a resolved face's own sector is a DOOR, the binding moves to the
	// SISTER face instead, provided the sister's sector is not also a door. A
	// trigger on a doorway belongs to the room, not to the moving leaf.
	{
		for (Command &c : map.commands)
		{
			if (c.key == 0) continue;          // "what the player just used"

			for (size_t si = 0; si < map.sectors.size(); si++)
			{
				if (map.sectors[si].commandID == c.key) { c.sector = (int)si; break; }
			}

			for (size_t fi = 0; fi < map.faces.size(); fi++)
			{
				const int tmi = map.faces[fi].textureMap;
				if (tmi < 0 || tmi >= (int)map.textureMaps.size()) continue;
				const TextureMap &tm = map.textureMaps[tmi];
				if (!tm.extended || tm.faceID != c.key) continue;

				int use = (int)fi;
				const int own = map.faces[fi].sector;
				if (own >= 0 && own < (int)map.sectors.size()
					&& map.sectors[own].IsDoorCapable())
				{
					const int sis = map.faces[fi].sister;
					if (sis >= 0 && sis < (int)map.faces.size())
					{
						const int ss = map.faces[sis].sector;
						if (ss >= 0 && ss < (int)map.sectors.size()
							&& !map.sectors[ss].IsDoorCapable())
							use = sis;
					}
				}
				c.faces.push_back(use);
			}
		}
	}

	if (r.Bad())
		map.error = "truncated file";
	else
		RecentreWrappedMap(map);
	return map;
}

} // namespace roth
