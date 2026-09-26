//
// Realms of the Haunting .RAW map reader.
//
// Ported from a Python implementation validated against all 44 retail maps.
// Deliberately engine-free; see roth_raw.h.
//

#include "roth_raw.h"

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
		s.overrideHeight = r.I8();
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
	std::unordered_map<uint32_t, int> faceByOffset;
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
				o.unk0E = r.U16();
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
		if (!s.IsDoor() || s.faceCount != 4 || s.firstFaceIndex < 0) continue;
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

	if (r.Bad())
		map.error = "truncated file";
	return map;
}

} // namespace roth
