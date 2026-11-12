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
// The four bytes before the entry, when it says it has them. See the comment
// on `AnchorOffset` in the header for where the original reads these and what
// the two halves are for.
//
// The bounds test is not defensive decoration: the offset is a file offset read
// out of the FAT, so an entry claiming to start in the first four bytes of the
// file would send this reader off the front of the mapping. MEASURED: no
// retail entry does, but the reader does not get to assume that.
//
//==========================================================================

AnchorOffset Pack::ReadAnchor(int index) const
{
	AnchorOffset a;
	const FatEntry *e = Entry(index);
	if (e == nullptr || (e->flags1 & FAT_ANCHOR) == 0) return a;
	if (e->offset < 4 || (size_t)e->offset > mSize) return a;

	const uint32_t dw = RdU32(mData + e->offset - 4);
	a.x = (int16_t)(uint16_t)(dw & 0xffff);
	a.y = (int16_t)(uint16_t)(dw >> 16);
	a.present = true;
	return a;
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

Image Pack::ReadImage(int index, bool allFrames, bool runLength) const
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
		// THE OTHER VARIANT (frame count word 0xFFFE): run-length frames,
		// each chosen by its owner rather than by a timer -- a creature's
		// attack, pain and death, an impact. decode_das_anim_frame
		// (das_assets.c:1503-1580): the frame records start at block+0x1a
		// (entry+16, magic 0x17); each has +4 stride, +6 height, +8 the
		// frame count (first record), +0xc the offset of the next record,
		// +0x10 left margin, +0x12 span, +0x14 top margin, +0x16 rows, and
		// the run-length stream at +0x18 (a byte below 0xf1 is a pixel,
		// otherwise (b - 0xf0) copies of the next byte). A record with one
		// row or a one-pixel span is an empty frame.
		if (runLength && e->offset + 18 <= mSize && RdU16(mData + e->offset + 12) == 0xFFFE)
		{
			const size_t base = e->offset;
			size_t p = base + 16;
			if (p + 0x18 <= mSize && RdU16(mData + p) == 0x17)
			{
				int count = RdU16(mData + p + 8);
				// THE LOADER'S REPAIR (repair_das_rle_frame_count, renderer.c
				// :10187, called from postprocess_loaded_das_block, das_assets.c
				// :792-794, when the picture word has 0x100 and the frame word
				// 0xfffe): it counts the frames whose record says the same frame
				// count and links on to a next record inside the block, and
				// writes that count back. The last record's link is 0, so a
				// list of N frames is played as N - 1 -- the ghoul's attack has
				// 20 records and the original shows 19 (measured: its
				// select_surface_anim_frame reads 19). The block is the FAT size
				// word times 2 (select_das_fat_entry, renderer.c:742-744), times 8
				// more for the frame kinds (das_assets.c:893-898).
				if ((RdU16(mData + base) & 0x100) && (RdU16(mData + p) & 0x10))
				{
					const size_t bytes = (size_t)e->size * 2u * ((e->flags2 & 3) ? 8u : 1u);
					const size_t end = p + (bytes > 0x20 ? bytes - 0x20 : 0);
					int valid = 0;
					size_t q = p;
					while (valid < count && q + 0x10 <= mSize)
					{
						if (RdU16(mData + q + 8) != (uint16_t)count) break;
						const int32_t next = (int32_t)RdU32(mData + q + 0xc);
						if (next < 1) break;
						q += (size_t)next;
						if (q > end) break;
						valid++;
					}
					count = valid;
				}
				const int W = RdU16(mData + p + 4), H = RdU16(mData + p + 6);
				img.modifier = mData[base];
				img.imageType = mData[base + 1];
				img.width = W; img.height = H;
				img.rle = true;
				img.ticksPerFrame = RdU16(mData + base + 10);
				for (int f = 0; f < count && f < 256 && W > 0 && H > 0; f++)
				{
					if (p + 0x18 > mSize) break;
					std::vector<uint8_t> out((size_t)W * H, 0);
					const int left = RdU16(mData + p + 0x10), span = RdU16(mData + p + 0x12);
					const int top = RdU16(mData + p + 0x14), rows = RdU16(mData + p + 0x16);
					if (rows != 1 && span != 1)
					{
						size_t o = (size_t)top * W + left, src = p + 0x18;
						for (int r = 0; r < rows; r++)
						{
							int n = span;
							while (n > 0 && src < mSize)
							{
								const uint8_t a = mData[src++];
								if (a < 0xf1) { if (o < out.size()) out[o] = a; o++; n--; }
								else
								{
									const int c = a - 0xf0;
									n -= c;
									const uint8_t v = src < mSize ? mData[src++] : 0;
									for (int k = 0; k < c; k++) { if (o < out.size()) out[o] = v; o++; }
								}
							}
							o += (size_t)(W - span);
						}
					}
					img.frames.push_back(std::move(out));
					if (!allFrames) break;
					const uint32_t next = RdU32(mData + p + 0xc);
					if (next == 0) break;
					p += next;
				}
			}
			break;
		}
		for (int attempt = 0; attempt < 2 && !img.ok(); attempt++)
		{
			size_t base = e->offset + (attempt ? 4 : 0);
			if (base + 18 > mSize) continue;
			uint16_t firstOff = RdU16(mData + base + 10);
			uint16_t frameCount = RdU16(mData + base + 12);
			if (frameCount == 0xFFFE) continue;     // the other variant
			img.timer = mData[base + 16];
			img.rate = mData[base + 17];
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

//==========================================================================
//
// One picture of an image pack, by the index a mesh face carries.
//
// The original's rule, renderer.c:13251-13273. The resolved art block's flag
// word has bit 0x40 (our `modifier & IM_IMAGE_PACK`); the word at block+0x10 --
// entry+6 here, because a block starts ten bytes before its entry -- says how a
// picture is chosen:
//
//     bit 15      by the view angle (a billboard's views)
//     bit 14      by the FACE: byte +0x1c of the mesh face record
//     neither     through a table of state words the engine keeps
//
// and in every case the chosen word, read from block+0x12+2k, is a paragraph
// offset from the block: the sub-record starts at block + word*16 and is laid
// out like a block of its own, so its flag word, dimensions and pixels sit at
// +0x0a, +0x0c/+0x0e and +0x10 -- which is the shape readPlainAt already reads
// when handed sub+0x0a. Bit 15 of the word asks for a mirrored draw
// (renderer.c:13271); MEASURED by tools/propcheck over all five map packs, no
// face-indexed table word sets it.
//
// The low bits of the selector word are the table's size in bytes: 0x401c is
// fourteen pictures. Only the bit-14 kind is resolved here.
//
//==========================================================================

int Pack::SubImageCount(int index) const
{
	const FatEntry *e = Entry(index);
	if (e == nullptr || e->kind != EntryKind::ImagePack) return 0;
	if ((size_t)e->offset + 8 > mSize) return 0;
	const uint16_t sel = RdU16(mData + e->offset + 6);
	if ((sel & 0x8000) != 0 || (sel & 0x4000) == 0) return 0;
	return (sel & 0x3fff) / 2;
}

Image Pack::ReadSubImage(int index, int sub) const
{
	const FatEntry *e = Entry(index);
	if (e == nullptr) return Image();
	if (e->kind != EntryKind::ImagePack) return ReadImage(index, false);

	const int n = SubImageCount(index);
	if (sub < 0 || sub >= n) return Image();
	const size_t at = (size_t)e->offset + 8 + (size_t)sub * 2;
	if (at + 2 > mSize) return Image();
	const uint16_t word = RdU16(mData + at);
	// block + (word & 0x7fff) * 16, plus 0x0a to the sub-record's flag word.
	// The block starts ten bytes before the entry, so the two tens cancel.
	const size_t p = (size_t)e->offset + (size_t)(word & 0x7fff) * 16;

	Image img;
	if (p + 6 > mSize) return img;
	const int w = RdU16(mData + p + 2);
	const int h = RdU16(mData + p + 4);
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return img;
	if (p + 6 + (size_t)w * h > mSize) return img;
	img.modifier = mData[p];
	img.imageType = mData[p + 1];
	img.width = w;
	img.height = h;
	img.frames.emplace_back(mData + p + 6, mData + p + 6 + (size_t)w * h);
	return img;
}

//==========================================================================
//
// A 3D prop. Layout, with the original's block offsets (block = entry - 10):
//
//   entry+20            vertex count                       (block+0x1e)
//   entry+22            vertices, 0x10 bytes each: x, up, y as int16 and ten
//                       bytes the original uses as per-frame scratch
//                       (render_world.c:653-676)
//   then 8 bytes        the four-character tag, two unused bytes, and the size
//                       of the face chain as a BIG-endian word
//   then the faces      a 0x36-byte record and a closed ring of corner words,
//                       each a BYTE OFFSET into the vertex table (so >> 4)
//
// The texture word at +0x0c is big-endian in an "EXP2" mesh and a plain byte in
// an "EXPL" one; initialize_das_block_internal_pointers normalises both at load
// (renderer.c:806, :815-821).
//
//==========================================================================

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
	mesh.expl = (mData[p] == 'E' && mData[p + 1] == 'X' && mData[p + 2] == 'P'
		&& mData[p + 3] == 'L');
	uint16_t facesSize = RdU16BE(mData + p + 6);   // big endian
	p += 8;

	size_t consumed = 4;
	while (consumed < facesSize && p + MESH_FACE_SIZE <= mSize)
	{
		MeshFace f;
		f.texture = mesh.expl ? (uint16_t)mData[p + 0x0C]      // renderer.c:819
		                      : RdU16BE(mData + p + 0x0C);     // renderer.c:821
		f.shade = mData[p + 0x0E];
		f.tile = mData[p + 0x0F];
		f.flags = RdU16(mData + p + 0x16);
		for (int i = 0; i < 12; i++) f.corner[i] = mData[p + 0x18 + i];
		f.subTexture = mData[p + 0x1C];
		f.extentV = RdU16(mData + p + 0x24);
		f.extentU = RdU16(mData + p + 0x26);
		uint16_t edgeCount = RdU16(mData + p + 0x34);
		p += MESH_FACE_SIZE;
		consumed += MESH_FACE_SIZE;

		int n = edgeCount + 1;
		if (n > 64 || p + (size_t)n * 2 > mSize) break;

		// A vertex index lives in the high bits of each corner word. The ring
		// is closed, so a quad is 5 words and a triangle 4; a count of zero
		// leaves the single word naming the vertex a picture hangs on.
		int idx[5] = { -1, -1, -1, -1, -1 };
		for (int i = 0; i < n && i < 5; i++)
			idx[i] = RdU16(mData + p + (size_t)i * 2) >> 4;
		p += (size_t)n * 2;
		consumed += (size_t)n * 2;

		if (n == 5)      { f.count = 4; for (int i = 0; i < 4; i++) f.vertex[i] = idx[i]; }
		else if (n == 4) { f.count = 3; for (int i = 0; i < 3; i++) f.vertex[i] = idx[i]; }
		else if (n == 1) { f.count = 0; f.vertex[0] = idx[0]; }
		else continue;

		bool bad = false;
		for (int i = 0; i < (f.count == 0 ? 1 : f.count); i++)
			if (f.vertex[i] < 0 || f.vertex[i] >= (int)mesh.vertices.size()) bad = true;
		if (!bad) mesh.faces.push_back(f);
	}

	return mesh;
}

//==========================================================================
//
// MeshFaceTexCoords -- see roth_das.h. One rule per span driver.
//
// THE AXES. Object art is stored with the picture's columns as its rows, so the
// original's "row index" is ACROSS the picture (u) and its "offset in the row"
// is DOWN it (v). The row count is the block's +0x0e word and the row length
// its +0x0c word (renderer.c:13288-13290); `texW` and `texH` are those two.
//
// WALL (neither 0x20 nor 0x200). classify_surface_floorceil hands the driver
// corner 0 as top-left, 1 as top-right, 2 as bottom-right and 3 as bottom-left
// (renderer.c:4283-4294). Across: the column walk runs the full u extent from
// the left edge to the right one, and the extent is two half-texels per texel
// times the repeat count in the LOW nibble of +0x0f (renderer.c:13363-13370,
// the `[0x90956]` word). Down: v is measured from corner 0's height and scaled
// so that one picture, times the HIGH nibble's repeat count, spans the face
// height the record stores at +0x24 (renderer.c:4979-4981, `[0x90954]` over
// `[0x90980]`). Flag bit 1 mirrors across, inside the texture's own columns
// (renderer.c:4735, `col = width - 1 - col`).
//   With flag 0x100 the record's own +0x24/+0x26 are the extents instead, at
// the wall density of two world units a texel (renderer.c:13347-13354 and
// :4975-4976).
//
// POLYGON (0x200). build_floorceil_vertex_records gives each corner of the ring
// a coordinate from a two-bit counter (renderer.c:3737-3750): down is 0, 0,
// max, max and across is max, 0, 0, max -- or 0, max, max, 0 when flag bit 1
// is set (renderer.c:3708-3710). With flag 0x1000 the corners carry their own
// coordinates as bytes, in texels: across at +0x1c+k, down at +0x20+k
// (renderer.c:3777-3793, where both end up shifted left six, the same
// fixed point as the `(dim << 6) - 1` extents above).
//
// PLANE (0x20). The sprite span driver maps a horizontal plane by scanline from
// the view's sine and cosine (renderer.c:3441-3453), so its picture is laid
// out in the object's ground plane rather than fitted to the ring. That
// derivation is NOT done here: the face is given the POLYGON rule with the
// repeat nibbles the shared builder applies under this flag
// (renderer.c:3695-3704), which fits the picture to the ring. 32 faces in the
// retail packs take this path; tools/propcheck lists the meshes.
//
//==========================================================================

bool MeshFaceTexCoords(const Mesh &mesh, const MeshFace &f, int texW, int texH,
	float u[4], float v[4])
{
	for (int i = 0; i < 4; i++) u[i] = v[i] = 0.f;
	if (f.count != 3 && f.count != 4) return false;
	if (texW <= 0 || texH <= 0) return false;
	for (int i = 0; i < f.count; i++)
		if (f.vertex[i] < 0 || f.vertex[i] >= (int)mesh.vertices.size()) return false;

	const bool mirror = (f.flags & 2) != 0;
	const float across = float(1 + (f.tile & 0x0f));
	const float down = float(1 + (f.tile >> 4));

	switch (f.Kind())
	{
	case MeshFaceKind::Wall:
	{
		const float top = float(mesh.vertices[f.vertex[0]].up);
		float uMax, vPerUnit;
		if (f.flags & 0x100)
		{
			uMax = float(f.extentU) / (2.f * float(texW));
			vPerUnit = 1.f / (2.f * float(texH));
		}
		else
		{
			uMax = across;
			// A record with no stored height cannot scale v; fall back to the
			// geometry, which is what the stored value is on every retail wall
			// face that has one (tools/propcheck, "wall extents").
			float h = float(f.extentV);
			if (h <= 0.f)
			{
				float lo = top;
				for (int i = 0; i < f.count; i++)
				{
					const float z = float(mesh.vertices[f.vertex[i]].up);
					if (z < lo) lo = z;
				}
				h = top - lo;
			}
			vPerUnit = (h > 0.f) ? down / h : 0.f;
		}
		// Corners 0 and 3 are the left edge, 1 and 2 the right.
		static const int rightEdge[4] = { 0, 1, 1, 0 };
		for (int i = 0; i < f.count; i++)
		{
			float uu = rightEdge[i] ? uMax : 0.f;
			if (mirror) uu = uMax - uu;
			u[i] = uu;
			v[i] = (top - float(mesh.vertices[f.vertex[i]].up)) * vPerUnit;
		}
		return true;
	}

	case MeshFaceKind::Polygon:
		if (f.flags & 0x1000)
		{
			for (int i = 0; i < f.count; i++)
			{
				u[i] = float(f.corner[4 + i]) / float(texW);
				v[i] = float(f.corner[8 + i]) / float(texH);
			}
			return true;
		}
		// fall through: the corner-counter rule, no repeats
	case MeshFaceKind::Plane:
	{
		const bool plane = (f.Kind() == MeshFaceKind::Plane);
		const float uMax = plane ? across : 1.f;
		const float vMax = plane ? down : 1.f;
		// The counter starts at 0xff and steps once per corner: across reads
		// bit 1 BEFORE the step, down reads it AFTER (renderer.c:3745-3748).
		for (int i = 0; i < f.count; i++)
		{
			const bool acrossHi = (((0xff + i) & 2) != 0);
			const bool downHi = (((0xff + i + 1) & 2) != 0);
			// [+0x24] is the extent and [+0x1c] zero with the bit clear; the
			// other way round with it set, and always under 0x20
			// (renderer.c:3701, :3708-3710).
			const bool swap = plane || mirror;
			u[i] = (acrossHi != swap) ? uMax : 0.f;
			v[i] = downHi ? vMax : 0.f;
		}
		return true;
	}

	default:
		return false;
	}
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
