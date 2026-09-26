//
// Realms of the Haunting .DAS artwork reader.
//
// Ported from a Python implementation validated against the retail packs.
// Engine-free; see roth_das.h.
//

#include "roth_das.h"

#include <string.h>

namespace roth
{

namespace
{

inline uint16_t RdU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t RdU32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
		| ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t RdU16BE(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// VGA 6-bit (0-63) to 8-bit, the same expansion the original uses.
inline uint8_t Expand6(uint8_t v) { return (uint8_t)((v * 259 + 33) >> 6); }

const size_t HEADER_SIZE = 68;
const size_t FAT_STRIDE = 8;
const size_t MESH_HEADER_SIZE = 22;
const size_t MESH_VERTEX_STRIDE = 0x10;
const size_t MESH_FACE_SIZE = 54;

} // namespace

bool Pack::Load(const uint8_t *data, size_t size)
{
	mData = data;
	mSize = size;
	mFat.clear();
	mPalette.clear();
	mError.clear();

	if (size < HEADER_SIZE || memcmp(data, "DASP", 4) != 0)
	{
		mError = "not a DAS pack";
		return false;
	}

	uint32_t fatOffset = RdU32(data + 8);
	uint32_t paletteOffset = RdU32(data + 12);
	mSkyIndex = RdU16(data + 34);

	// The image count is the sum of four block counts.
	int count = 0;
	for (int i = 0; i < 4; i++)
		count += RdU16(data + 0x30 + i * 2);
	if (count <= 0 || fatOffset + (size_t)count * FAT_STRIDE > size)
	{
		mError = "bad FAT";
		return false;
	}

	mFat.resize(count);
	for (int i = 0; i < count; i++)
	{
		const uint8_t *p = data + fatOffset + (size_t)i * FAT_STRIDE;
		FatEntry &e = mFat[i];
		e.offset = RdU32(p);
		e.size = RdU16(p + 4);
		e.flags1 = p[6];
		e.flags2 = p[7];
		e.kind = Classify(e);
	}

	// A palette offset of zero is not an error: it means "use the built-in
	// table", which the shared sprite pack relies on.
	mPalette.resize(256);
	const uint8_t *pal = DEFAULT_RAW_PALETTE;
	if (paletteOffset != 0 && paletteOffset + 768 <= size)
		pal = data + paletteOffset;
	for (int i = 0; i < 256; i++)
		mPalette[i] = { Expand6(pal[i * 3]), Expand6(pal[i * 3 + 1]),
						Expand6(pal[i * 3 + 2]) };

	return true;
}

const FatEntry *Pack::Entry(int index) const
{
	if (index < 0 || index >= (int)mFat.size()) return nullptr;
	return &mFat[index];
}

EntryKind Pack::Classify(const FatEntry &e) const
{
	if (e.size == 0 || e.offset == 0 || e.offset + 6 > mSize)
		return EntryKind::Empty;
	// These flags mean the entry points at a mapping table rather than holding
	// an image, so its bytes are not a picture header.
	if (e.flags1 & (FAT_MONSTER | FAT_DIRECTIONAL))
		return EntryKind::Indirection;

	uint8_t modifier = mData[e.offset];
	uint8_t imageType = mData[e.offset + 1];
	if (imageType & IT_OBJECT_DATA) return EntryKind::Object3D;
	if (imageType & IT_ANIMATED)    return EntryKind::Animated;
	if (modifier & IM_IMAGE_PACK)   return EntryKind::ImagePack;
	return EntryKind::Plain;
}

Image Pack::ReadImage(int index, bool allFrames) const
{
	Image img;
	const FatEntry *e = Entry(index);
	if (!e) return img;

	auto readPlainAt = [&](size_t p) -> bool
	{
		if (p + 6 > mSize) return false;
		int w = RdU16(mData + p + 2);
		int h = RdU16(mData + p + 4);
		if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
		if (p + 6 + (size_t)w * h > mSize) return false;
		img.modifier = mData[p];
		img.imageType = mData[p + 1];
		img.width = w;
		img.height = h;
		img.frames.emplace_back(mData + p + 6, mData + p + 6 + (size_t)w * h);
		return true;
	};

	switch (e->kind)
	{
	case EntryKind::Plain:
		readPlainAt(e->offset);
		break;

	case EntryKind::Animated:
	{
		// Header, then frame offsets, then padding, then a small header and the
		// first frame uncompressed. Packs using the built-in palette prefix the
		// entry with 4 bytes, so try both starts and take the plausible one.
		for (int attempt = 0; attempt < 2 && !img.ok(); attempt++)
		{
			size_t base = e->offset + (attempt ? 4 : 0);
			if (base + 18 > mSize) continue;
			uint16_t firstOff = RdU16(mData + base + 10);
			uint16_t nsub = RdU16(mData + base + 12);
			if (nsub == 0xFFFE) continue;           // the other variant
			if (!readPlainAt(base + firstOff)) continue;

			if (allFrames && nsub > 0)
			{
				// Each later frame is a stream of edits applied to the one
				// before it, in place.
				size_t p = base + firstOff + 6
					+ (size_t)img.width * img.height;
				std::vector<uint8_t> frame = img.frames[0];
				for (int f = 0; f < nsub && f < 64; f++)
				{
					size_t pos = 0;
					bool done = false;
					while (p < mSize && !done)
					{
						uint8_t code = mData[p++];
						if (code == 0)
						{
							if (p >= mSize) break;
							code = mData[p++];
							if (code == 0) { done = true; break; }
							if (p >= mSize) break;
							uint8_t value = mData[p++];
							for (int k = 0; k < code; k++)
								if (pos + k < frame.size()) frame[pos + k] = value;
							pos += code;
						}
						else if (code > 0x80)
						{
							pos += code & 0x7F;     // leave these bytes alone
						}
						else
						{
							for (int k = 0; k < code; k++, p++)
								if (p < mSize && pos + k < frame.size())
									frame[pos + k] = mData[p];
							pos += code;
						}
						if (pos > frame.size()) { done = true; break; }
					}
					img.frames.push_back(frame);
				}
			}
		}
		break;
	}

	case EntryKind::ImagePack:
	{
		// Two variants. The directional one starts its images at a fixed offset
		// from the entry; the other has a zero-terminated table first.
		if (e->offset + 8 <= mSize && (mData[e->offset + 7] & 0x80))
		{
			readPlainAt(e->offset + 32);
		}
		else
		{
			size_t p = e->offset + 4;
			while (p + 2 <= mSize && RdU16(mData + p) != 0) p += 2;
			p += 2;
			while (p < mSize && mData[p] == 0) p++;
			if (p > 0) readPlainAt(p - 1);
		}
		break;
	}

	default:
		break;
	}

	return img;
}

Mesh Pack::ReadMesh(int index) const
{
	Mesh mesh;
	const FatEntry *e = Entry(index);
	if (!e || e->kind != EntryKind::Object3D) return mesh;
	if (e->offset + MESH_HEADER_SIZE > mSize) return mesh;

	size_t p = e->offset;
	uint16_t nverts = RdU16(mData + p + 20);
	if (nverts == 0 || nverts > 4096) return mesh;
	p += MESH_HEADER_SIZE;

	mesh.vertices.reserve(nverts);
	for (int i = 0; i < nverts; i++)
	{
		if (p + MESH_VERTEX_STRIDE > mSize) return Mesh();
		Mesh::Vertex v;
		v.x  = (int16_t)RdU16(mData + p);
		v.up = (int16_t)RdU16(mData + p + 2);   // MIDDLE value is vertical
		v.y  = (int16_t)RdU16(mData + p + 4);
		mesh.vertices.push_back(v);
		p += MESH_VERTEX_STRIDE;
	}

	if (p + 8 > mSize) return Mesh();
	uint16_t facesSize = RdU16BE(mData + p + 6);   // big endian
	p += 8;

	size_t consumed = 4;
	while (consumed < facesSize && p + MESH_FACE_SIZE <= mSize)
	{
		MeshFace f;
		f.texture = RdU16BE(mData + p + 0x0C);     // big endian
		f.flipV = (mData[p + 0x16] & 2) != 0;
		f.subTexture = mData[p + 0x1C];
		uint16_t edgeCount = RdU16(mData + p + 0x34);
		p += MESH_FACE_SIZE;
		consumed += MESH_FACE_SIZE;

		int n = edgeCount + 1;
		if (n > 64 || p + (size_t)n * 2 > mSize) break;

		// A vertex index lives in the high bits of each edge word. Realms
		// describes a quad with 5 edges and a triangle with 4.
		int idx[5];
		for (int i = 0; i < n && i < 5; i++)
			idx[i] = RdU16(mData + p + (size_t)i * 2) >> 4;
		p += (size_t)n * 2;
		consumed += (size_t)n * 2;

		if (n == 5)      { f.count = 4; for (int i = 0; i < 4; i++) f.vertex[i] = idx[i]; }
		else if (n == 4) { f.count = 3; for (int i = 0; i < 3; i++) f.vertex[i] = idx[i]; }
		else continue;

		bool bad = false;
		for (int i = 0; i < f.count; i++)
			if (f.vertex[i] < 0 || f.vertex[i] >= (int)mesh.vertices.size()) bad = true;
		if (!bad) mesh.faces.push_back(f);
	}

	return mesh;
}

bool Pack::ResolveObjectArt(uint8_t textureIndex, uint8_t textureSource, int &outIndex)
{
	switch (textureSource & 3)
	{
	case 0: outIndex = textureIndex + 4096;       return false;
	case 1: outIndex = textureIndex + 4096 + 256; return false;
	case 2: outIndex = textureIndex;              return true;
	default: outIndex = textureIndex + 256;       return true;
	}
}

} // namespace roth
