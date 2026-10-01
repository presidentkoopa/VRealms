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
	mUnknown0x22 = RdU16(data + 34);   // the sky MARKER index -- see roth_das.h

	// The directional-object block. A pack without one simply has no
	// view-dependent art; that is not an error, so a bad extent disables the
	// block rather than failing the load.
	mDirOffset = RdU32(data + 0x1c);
	mDirSize = RdU16(data + 0x1a);
	if (mDirOffset == 0 || (size_t)mDirOffset + mDirSize > size)
	{
		mDirOffset = 0;
		mDirSize = 0;
	}

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
	mShade = nullptr;
	mFogIndex = 0;
	mGlow = nullptr;
	if (paletteOffset != 0 && (size_t)paletteOffset + 768 + 2 + 0x4000 <= size)
	{
		mFogIndex = data[paletteOffset + 768];
		mShade = data + paletteOffset + 768 + 2;
	}
	// THE GLOW TABLE, one 256-byte palette remap row.
	//
	// The original reads five blocks in a row after the palette
	// (map_load.c:371-380): the 2-byte remap prefix, 0x4000 of shade ramps,
	// a 0x10000 translucency LUT, 0x100 of something unrelated, and then this.
	// So it lands at palette + 0x300 + 2 + 0x4000 + 0x10000 + 0x100.
	//
	// It is not a shade ramp and has no rows: a glowing surface is drawn as
	// glow[texel] with NO depth term, NO sector light and NO flash bonus, so it
	// ignores lighting entirely. See roth_palshade.
	{
		const size_t glowAt = (size_t)paletteOffset + 0x14402;
		if (paletteOffset != 0 && glowAt + 256 <= size) mGlow = data + glowAt;
	}
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

//==========================================================================
//
// Which indirection an entry is.
//
// The original tests the WHOLE of flags_1, not single bits (das_assets.c:886):
// 0x20 stamps the status word 0xfe and resolves through the per-map
// directional table; 0x24 stamps 0xfc and spawns a live actor. An entry
// carrying either bit alongside others takes neither path -- it is loaded as
// an ordinary picture. Classify() groups both under Indirection, which is
// enough to know "not a picture"; this says which kind.
//
//==========================================================================

IndirectKind Pack::Indirect(int index) const
{
	const FatEntry *e = Entry(index);
	if (e == nullptr) return IndirectKind::None;
	if (e->flags1 == 0x20) return IndirectKind::Directional;
	if (e->flags1 == 0x24) return IndirectKind::Creature;
	return IndirectKind::None;
}

//==========================================================================
//
// Resolve a directional entry to one picture per view. See the comment on
// `Directional` in the header for the original's frame pick.
//
// The record is found by the entry's flags_2 byte, not by its FAT index: the
// loader stamps flags_2 into the high byte of the status word, and the
// renderer reads it back from there as the table index (das_assets.c:888,
// renderer.c:5890). That indirection is the piece that connects an object to
// its frames, and nothing else in this reader needed flags_2 before.
//
// The view count comes from the record's own first word, so a pack may mix
// eight- and sixteen-view entries.
//
//==========================================================================

Directional Pack::ReadDirectional(int index) const
{
	Directional d;
	if (mDirSize == 0 || Indirect(index) != IndirectKind::Directional)
		return d;

	const uint8_t *block = mData + mDirOffset;
	const size_t aid = (size_t)Entry(index)->flags2;
	if (aid * 2 + 2 > mDirSize) return d;

	const size_t off = RdU16(block + aid * 2);
	if (off + 2 > mDirSize) return d;

	const uint8_t *rec = block + off;
	const uint16_t w = RdU16(rec);

	// Bit 15 clear means a fixed frame chosen through a table the original
	// keeps in engine state rather than in the file (renderer.c:5915, the same
	// table as the resident-block path at :5708). Nothing in the file resolves
	// it, so report the entry as unresolved rather than guessing an index.
	if (!(w & 0x8000)) return d;

	const int count = (w & 0x2000) ? 16 : 8;
	if (off + 2 + (size_t)count * 2 > mDirSize) return d;

	for (int i = 0; i < count; i++)
	{
		const uint16_t fw = RdU16(rec + 2 + (size_t)i * 2);
		d.frames[i].entry = (uint16_t)(fw & 0x7fff);
		d.frames[i].mirror = (fw & 0x8000) != 0;
	}
	d.count = count;
	return d;
}

//==========================================================================
//
// One animation frame's edit stream, applied in place to the previous frame.
//
// Transcribed from apply_das_sprite_frame_delta_stream (renderer.c:834). The
// opcode set is small and every case matters:
//
//   0x00 c v      fill the next c bytes with v        (c == 0 is legal, not an end)
//   0x01..0x7f    copy that many literal bytes
//   0x81..0xff    skip (code - 0x80) bytes, leaving the previous frame's
//   0x80 lo hi    hi == 0x00      END OF STREAM
//                 hi <  0x80      skip the 16-bit count
//                 hi <  0xc0      copy ((hi - 0x80) << 8 | lo) literal bytes
//                 else            fill ((hi & 0x3f) << 8 | lo) bytes with the next byte
//
// The version this replaces had neither the 0x80 escape nor the real
// terminator: it read 0x80 as a 128-byte literal and stopped on a 0x00 0x00
// pair, which is an ordinary zero-length fill. Bounds are checked here because
// the source is a stranger's file; the original trusts it.
//
//==========================================================================

void Pack::ApplyFrameDelta(std::vector<uint8_t> &frame, size_t p) const
{
	size_t pos = 0;
	const size_t end = frame.size();

	auto fill = [&](size_t count, uint8_t value)
	{
		for (size_t k = 0; k < count && pos + k < end; k++) frame[pos + k] = value;
		pos += count;
	};
	auto literal = [&](size_t count) -> bool
	{
		if (p + count > mSize) return false;
		for (size_t k = 0; k < count && pos + k < end; k++) frame[pos + k] = mData[p + k];
		p += count;
		pos += count;
		return true;
	};

	while (p < mSize)
	{
		uint8_t code = mData[p++];
		if (code == 0x00)
		{
			if (p + 2 > mSize) return;
			uint8_t count = mData[p], value = mData[p + 1];
			p += 2;
			fill(count, value);
		}
		else if (code < 0x80)
		{
			if (!literal(code)) return;
		}
		else if (code != 0x80)
		{
			pos += (size_t)(code - 0x80);
		}
		else
		{
			if (p + 2 > mSize) return;
			uint8_t lo = mData[p], hi = mData[p + 1];
			p += 2;
			if (hi == 0x00) return;                       // the terminator
			const uint16_t word = (uint16_t)(lo | (hi << 8));
			if (hi < 0x80) pos += word;
			else if (hi < 0xC0)
			{
				if (!literal((size_t)(((hi - 0x80) << 8) | lo))) return;
			}
			else
			{
				if (p >= mSize) return;
				fill((size_t)(((hi & 0x3F) << 8) | lo), mData[p++]);
			}
		}
		// A stream that walks past the end of the frame is corrupt; the
		// writes are clamped above, so stopping here just avoids spinning.
		if (pos > end) return;
	}
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
		//------------------------------------------------------------------
		// A STORED ENTRY IS A CACHE BLOCK MINUS ITS FIRST TEN BYTES.
		//
		// The original reads a FAT entry into a block whose leading 10 bytes it
		// fills in itself, so every offset it quotes as `block + N` is byte
		// N - 10 of the stored entry:
		//
		//   block+0x0a  flags word    = entry+0    (modifier | image_type << 8)
		//   block+0x0c  width         = entry+2
		//   block+0x0e  height        = entry+4
		//   block+0x14  frame offset  = entry+10
		//   block+0x16  frame count   = entry+12   (0xFFFE = the RLE variant)
		//   block+0x1a  timer, rate   = entry+16
		//   block+0x1c  DELTA TABLE   = entry+18   one dword per frame
		//
		// das_assets.c:1479-1488 is the whole animation step:
		//
		//   framep = block + word[block+0x14] + 0x10;
		//   delta  = dword[block + oldFrame*4 + 0x1c];
		//   if (delta) apply_delta(framep, block + delta);
		//
		// So frame 0's pixels are at entry + frameOffset + 6, and each frame's
		// edit stream is found THROUGH THE TABLE -- not by running on from
		// wherever the previous one stopped. A table entry of 0 means "no
		// change from the previous frame"; the original just skips the call.
		//
		// Cross-checked against all 57 animated entries in DEMO.DAS: table[0]
		// is always exactly frameOffset + 6 + width*height + 10, so the first
		// stream does sit right behind the pixels -- which is why the old
		// reader got frame 1 right and then drifted -- every stream terminates
		// cleanly inside the entry, and 334 of the 346 consecutive stream pairs
		// end exactly where the next one starts.
		//------------------------------------------------------------------
		for (int attempt = 0; attempt < 2 && !img.ok(); attempt++)
		{
			size_t base = e->offset + (attempt ? 4 : 0);
			if (base + 18 > mSize) continue;
			uint16_t firstOff = RdU16(mData + base + 10);
			uint16_t frameCount = RdU16(mData + base + 12);
			if (frameCount == 0xFFFE) continue;     // the other variant
			if (!readPlainAt(base + firstOff)) continue;

			// THE FLAGS BELONG TO THE ENTRY, NOT TO THE FRAME. readPlainAt has
			// just taken modifier/imageType from the frame's own little header,
			// which is not where the original looks: block+0x0a -- entry+0 --
			// is the flags word, and the animation's frames do not carry their
			// own. Taking the frame's zeroes cost two visible bugs on the same
			// object, DEMO[4141], an animated suit of armour:
			//
			//   modifier 0x80 lost -> no size modifier -> two world units per
			//     texel instead of one -> drawn 398 units tall against a
			//     154-unit player, rather than its real 199
			//   image_type 0x17 lost -> the translucency bit with it -> the
			//     transparent area around the figure drawn as solid colour
			//     instead of holes, so the wallpaper behind it came through as
			//     coloured streaks
			img.modifier  = mData[base];
			img.imageType = mData[base + 1];

			// The original wraps the frame number on the count, so frames
			// 0 .. count-1 are the whole loop and the LAST table entry is the
			// wrap back to frame 0 -- which a forward animation must not apply.
			if (!allFrames || frameCount <= 1) break;
			if (base + 18 + (size_t)frameCount * 4 > mSize) break;

			std::vector<uint8_t> frame = img.frames[0];
			for (int f = 0; f + 1 < (int)frameCount && f < 63; f++)
			{
				uint32_t delta = RdU32(mData + base + 18 + (size_t)f * 4);
				// Block-relative, and the block starts 10 bytes before the
				// entry, so anything under 10 cannot be a real stream.
				if (delta >= 10) ApplyFrameDelta(frame, base + delta - 10);
				img.frames.push_back(frame);
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
	// (see ResolveDasId below for ids that arrive without a pack selector)
	default: outIndex = textureIndex + 256;       return true;
	}
}

bool Pack::ResolveDasId(int id, int &outIndex)
{
	if (id >= SHARED_ID_BASE) { outIndex = id - SHARED_ID_BASE; return true; }
	outIndex = id;
	return false;
}

} // namespace roth
