//
// Realms of the Haunting's GDV movies. See roth_gdv.h for what is here and
// where each rule comes from. Engine-free.
//

#include "roth_gdv.h"

#include <string.h>

namespace roth
{

namespace
{

inline uint16_t Rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t Rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint32_t Ror(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
inline uint32_t Rol(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

// The coders' bit reader (gdv_cutscene.c:392-412): low bits first out of a
// 32-bit accumulator; when the running count drops to 0 or below, 16 more
// bits are slid in above the ones still unread. The byte reads of the
// coders share its cursor.
struct Bits
{
	uint32_t acc = 0;
	int32_t left = 0;
	const uint8_t *p = nullptr;

	void Start(const uint8_t *payload)
	{
		acc = Rd32(payload);
		p = payload + 4;
		left = 0x10;
	}
	void Refill()
	{
		left += 0x10;
		const uint32_t cl = (uint32_t)left & 0x1f;
		acc = Ror(acc, cl);
		acc = (acc & 0xffff0000u) | Rd16(p);
		p += 2;
		acc = Rol(acc, cl);
	}
	uint32_t Take(int n)
	{
		const uint32_t v = acc & ((1u << n) - 1u);
		acc >>= n;
		left -= n;
		if (left <= 0) Refill();
		return v;
	}
	uint8_t Byte() { return *p++; }

	// The literal run's length (gdv_cutscene.c:449-468): a unary code in
	// which each step reads `cl` bits but keeps only the low 8 of the
	// accumulator, as the original's `mov al, dl` does, so it can never
	// grow past nine steps.
	uint32_t RunLength()
	{
		uint32_t len = 2, cl = 1, mask = 1;
		for (;;)
		{
			const uint8_t low = (uint8_t)acc;
			acc >>= cl;
			left -= (int32_t)cl;
			if (left <= 0) Refill();
			const uint32_t v = (uint32_t)low & mask;
			len += v;
			if (v != mask) return len;
			cl++;
			mask = mask * 2 + 1;
		}
	}
};

// Copy `n` bytes forward: an odd byte first, then two at a time, each pair
// read before it is written (rep movsw), so a source one byte behind the
// destination repeats a two-byte pattern (gdv_cutscene.c:417-422).
inline uint8_t *CopyWords(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	if (n & 1) *dst++ = *src++;
	for (uint32_t w = n >> 1; w; w--)
	{
		const uint8_t a = src[0], b = src[1];
		dst[0] = a; dst[1] = b;
		dst += 2; src += 2;
	}
	return dst;
}

// rep movsb (gdv_cutscene.c:423-424).
inline uint8_t *CopyBytes(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	while (n--) *dst++ = *src++;
	return dst;
}

// Repeat the byte before the destination (gdv_cutscene.c:425-426).
inline uint8_t *RepeatPrev(uint8_t *dst, uint32_t n)
{
	const uint8_t b = dst[-1];
	if (n & 1) *dst++ = b;
	for (uint32_t w = n >> 1; w; w--) { dst[0] = b; dst[1] = b; dst += 2; }
	return dst;
}

// The {code, width, height} table gdv_read_file_header searches when the
// header's height is 0 (gdv_cutscene.c:1281-1291; g_rng_state + 4, 0x72770).
// ROTH.EXE's own initialised data, read out of the running original by
// tools/gdv_test/gdvprobe.c ("D" lines); the retail movies all carry their
// size, so this is only the original's fallback.
struct SizeCode { uint16_t code, w, h; };
const SizeCode kSizeCodes[] = {
#include "roth_gdv_sizes.h"
};

// The slack after the picture: the original leaves 0x109-0x110 bytes before
// the chunk ring (gdv_cutscene.c:912-917). A coder that runs further than
// that in a damaged chunk is stopped here instead of writing over memory.
const uint32_t SLACK = 0x10000;
// The zeros after a chunk's payload copy (DecodeChunk).
const uint32_t PAD = 0x2000;

} // namespace

//==========================================================================
//
// Open: gdv_read_file_header (gdv_cutscene.c:1237-1333),
// gdv_setup_decode_buffers (:877-931), gdv_init_pixel_tables (:66-97).
//
//==========================================================================

bool GdvDecoder::Open(const uint8_t *data, size_t size, uint32_t streamFlags)
{
	*this = GdvDecoder();
	mData = data;
	mSize = size;
	mStreamFlags = streamFlags;
	if (data == nullptr || size < 0x18) return Fail("shorter than a GDV header");
	if (Rd32(data) != 0x29111994u) return Fail("not a GDV file (magic)");      // :1267

	// The header buffer: 0x18 bytes, the palette at +0x18 and its copy at
	// +0x318 (:1269-1277).
	mHeader.assign(0x18 + 0x600, 0);
	memcpy(mHeader.data(), data, 0x18);
	mPos = 0x18;
	mVersion = Rd16(data + 0xe);
	if (mVersion == 1)
	{
		if (size < 0x18 + 0x300) return Fail("the palette is cut off");
		memcpy(mHeader.data() + 0x18, data + 0x18, 0x300);
		memcpy(mHeader.data() + 0x318, data + 0x18, 0x300);
		mPos += 0x300;
	}

	// Height 0: the size code's table entry (:1281-1291).
	if (Rd16(mHeader.data() + 0x16) == 0)
	{
		const uint16_t code = Rd16(mHeader.data() + 4);
		const SizeCode *hit = nullptr;
		for (const SizeCode &s : kSizeCodes) if (s.code == code) { hit = &s; break; }
		if (hit == nullptr) return Fail("height 0 and a size code the table does not hold");
		mHeader[0x14] = (uint8_t)hit->w; mHeader[0x15] = (uint8_t)(hit->w >> 8);
		mHeader[0x16] = (uint8_t)hit->h; mHeader[0x17] = (uint8_t)(hit->h >> 8);
	}
	mWidth = Rd16(mHeader.data() + 0x14);
	mHeight = Rd16(mHeader.data() + 0x16);
	mFrameCount = Rd16(mHeader.data() + 6);
	mFps = Rd16(mHeader.data() + 8);
	mRate = Rd16(mHeader.data() + 0xc);
	mAudioFlags = Rd16(mHeader.data() + 0xa);
	mMaxChunk = Rd16(mHeader.data() + 0x10);

	// The audio block sizes (:1293-1316). The rate is divided by the frame
	// rate in 16 bits; a silent movie still runs its clock at 0x5622/12.
	uint32_t a = 0;
	if (mAudioFlags != 0)
	{
		a = (uint16_t)mRate;
		if (mFps != 0) a = (uint16_t)(a / (uint16_t)mFps);
	}
	mSamplesPerFrame = a != 0 ? a : 0x5622u / 0xcu;
	const uint8_t af = (uint8_t)mAudioFlags;
	if (af & 2) a += a;
	if (af & 4) a += a;
	if (af & 8) a >>= 1;
	mAudioBytes = a;

	// Pixel format and the decode buffer (:1318-1331).
	if (mVersion != 1 && mVersion != 2) return Fail("unknown GDV version");
	mBpp = mVersion;
	mPixelBytes = ((uint32_t)mWidth * (uint32_t)mHeight * (uint32_t)mBpp + 3u) & ~3u;
	mGuard = 0x1000u * (uint32_t)mBpp;
	if (mWidth <= 0 || mHeight <= 0 || mPixelBytes == 0) return Fail("empty picture");

	// gdv_setup_decode_buffers' DPCM table, built when the audio is DPCM
	// (:895-903, gdv_build_scale_table :846-862): entry 0 is 0, then pairs
	// (+v, -v) of a growing step, then one last +v.
	if (af & 8)
	{
		uint32_t e = 0x40, d = 0x2d, v = 0;
		mStep[0] = 0;
		int i = 1;
		for (; i < 255; i += 2)
		{
			v += e >> 5;
			e += d;
			d += 2;
			mStep[i] = (int32_t)v;
			mStep[i + 1] = -(int32_t)v;
		}
		v += e >> 5;
		mStep[255] = (int32_t)v;
	}

	// gdv_init_pixel_tables (:66-97): the colour table in the guard before
	// the picture (8-bit: two passes of 256 eight-byte runs of the index;
	// 16-bit: 0x400 pairs stepping 0x200020), "clear before the next
	// chunk", and an empty picture. gdv_alloc_decode_buffer zeroed it all
	// first (:2340-2342).
	mBuf.assign(mGuard + mPixelBytes + SLACK, 0);
	uint8_t *t = mBuf.data();
	if (mVersion == 2)
	{
		uint32_t v = 0;
		for (int i = 0; i < 0x400; i++)
		{
			for (int k = 0; k < 2; k++) { t[0] = (uint8_t)v; t[1] = (uint8_t)(v >> 8); t[2] = (uint8_t)(v >> 16); t[3] = (uint8_t)(v >> 24); t += 4; }
			v += 0x200020u;
		}
	}
	else
	{
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < 256; i++) { memset(t, i, 8); t += 8; }
	}
	mDeb = 0xff;
	mDed = 0;
	return true;
}

//==========================================================================
//
// One record.
//
//==========================================================================

bool GdvDecoder::NextFrame()
{
	if (mData == nullptr) return false;
	if (mFrameIndex + 1 >= mFrameCount) return false;
	// gdv_read_frame_chunk (gdv_cutscene.c:1381-1400): the audio block and the
	// chunk header in one read, then the payload. A maximum chunk size of 0
	// means no chunk at all follows the audio.
	const size_t rec = mPos;
	if (rec + mAudioBytes + 8 > mSize) return Fail("the movie ends in the middle of a record");
	const uint8_t *chunk = mData + rec + mAudioBytes;
	uint32_t payload = 0;
	if (mMaxChunk != 0)
	{
		if (Rd16(chunk) != 0x1305) return Fail("a chunk without its 0x1305 magic");       // :1392
		payload = Rd16(chunk + 2);
		if (payload > mMaxChunk) return Fail("a chunk larger than the header allows");   // :1395
		if (rec + mAudioBytes + 8 + payload > mSize) return Fail("a chunk is cut off");
	}
	mPos = rec + mAudioBytes + 8 + payload;
	mFrameIndex++;

	mPaletteWrites = 0;
	if (!mVideo)
	{
		mClock += mSamplesPerFrame;
		DecodeAudio(mData + rec);
		return true;
	}
	if (!DecodeChunk(chunk)) return false;
	DecodeAudio(mData + rec);
	mWritesDecode = mPaletteWrites;

	if (mFrameIndex == 0)
	{
		// gdv_prime_first_frame (:1772-1783): the settle count is 0 while the
		// first frame goes up and 1 after it; then the loop's palette upload.
		mDe4 = 0;
		Emit();
		mDe4 = 1;
		LoopStartPalette();
	}
	else Emit();
	mWritesEmit = mPaletteWrites - mWritesDecode;
	return true;
}

//==========================================================================
//
// gdv_decode_video_chunk (gdv_cutscene.c:760-814).
//
//==========================================================================

bool GdvDecoder::DecodeChunk(const uint8_t *chunk)
{
	mClock += mSamplesPerFrame;                                 // :763
	if (Rd16(chunk) != 0x1305) return Fail("a chunk without its 0x1305 magic");   // :768-771
	const uint32_t flags = Rd32(chunk + 4);
	const uint32_t sub = flags & 0xf;

	if (mDeb != 0 || (chunk[4] & 0x40))                         // :775-779
	{
		ClearPixels(0);
		mDeb = 0;
		mDed = 0;
	}
	mDc0 = (uint8_t)flags;                                      // :780
	if (flags & 0x10) mDe2 |= 4; else mDe2 &= (uint8_t)~4;      // :781
	if (flags & 0x20) mDe2 |= 8; else mDe2 &= (uint8_t)~8;      // :782
	if (mDe2 != mDe3)                                           // :783-786
	{
		if (mDed != 0) Reformat();
		mDe3 = mDe2;
	}
	mDed = 1;                                                   // :787

	// The coders read the payload with no end check of their own (a valid
	// stream ends itself; the bit reader runs at most two bytes ahead). They
	// read it here from a copy with zeros after it, so a damaged chunk can
	// only stop with an error, never read past the movie.
	const uint32_t paysz = Rd16(chunk + 2);
	mScratch.assign((size_t)paysz + PAD, 0);
	memcpy(mScratch.data(), chunk + 8, paysz);
	mReadLimit = mScratch.data() + paysz + PAD / 2;
	const uint8_t *payload = mScratch.data();
	if (mVersion == 1)                                          // :795-804
	{
		switch (sub)
		{
		case 8: Fmt8(payload, flags); break;
		case 6: Fmt6(payload, flags); break;
		case 5: Fmt5(payload, flags); break;
		case 2: Fmt2(payload); break;
		case 0: Fmt0(payload); break;
		case 1: Fmt1(chunk, flags); break;
		default: break;
		}
	}
	else                                                        // :805-811
	{
		switch (sub)
		{
		case 2: Fmt2Hi(payload); break;
		case 0: Fmt0(payload); break;
		case 1: Fmt1(chunk, flags); break;
		default: break;
		}
	}
	return mError.empty();
}

void GdvDecoder::ClearPixels(uint8_t value)
{
	memset(mBuf.data() + mGuard, value, mPixelBytes);
}

// gdv_reformat_pixel_buffer (gdv_cutscene.c:116-197): when a frame's half-
// resolution bits differ from the last frame's, the picture already in the
// buffer is re-laid-out for the new size, so the next delta lands right.
// The loops are the original's `dec; jg` loops: they run once even for a
// count of 0.
void GdvDecoder::Reformat()
{
	const uint8_t de2 = mDe2;
	const uint8_t chg = (uint8_t)(de2 ^ mDe3);
	uint8_t *buf = mBuf.data() + mGuard;
	const uint32_t bufsz = mPixelBytes;

	if (chg & 8)
	{
		const uint32_t w = (uint32_t)mWidth, h = (uint32_t)mHeight;
		if ((de2 & 8) == 0)                                     // :127-147 rows doubled
		{
			uint32_t W = w, sz = bufsz;
			if (de2 & 4) { sz = bufsz >> 1; W = w >> 1; }
			uint8_t *dst = buf + sz;
			const uint8_t *src = buf + (sz >> 1);
			uint32_t outer = (uint16_t)h >> 1;
			for (;;)
			{
				dst -= W;
				uint32_t inner = W >> 2;
				for (;;)
				{
					uint8_t v[4];
					memcpy(v, src - 4, 4);
					dst -= 4;
					memcpy(dst + W, v, 4);
					src -= 4;
					memcpy(dst, v, 4);
					const uint32_t io = inner; inner -= 1;
					if (inner == 0 || (int32_t)io <= 0) break;
				}
				const uint32_t oo = outer; outer -= 1;
				if (outer == 0 || (int32_t)oo <= 0) break;
			}
		}
		else                                                    // :148-165 every other row kept
		{
			uint32_t W = w;
			if (de2 & 4) W = w >> 1;
			const uint8_t *src = buf;
			uint8_t *dst = buf;
			uint32_t outer = (uint16_t)h >> 1;
			for (;;)
			{
				uint32_t inner = W >> 2;
				for (;;)
				{
					memmove(dst, src, 4);
					src += 4; dst += 4;
					const uint32_t io = inner; inner -= 1;
					if (inner == 0 || (int32_t)io <= 0) break;
				}
				src += W;
				const uint32_t oo = outer; outer -= 1;
				if (outer == 0 || (int32_t)oo <= 0) break;
			}
		}
	}

	if (chg & 4)
	{
		if ((de2 & 4) == 0)                                     // :169-182 columns doubled
		{
			uint32_t sz = bufsz;
			if (de2 & 8) sz = bufsz >> 1;
			uint8_t *dst = buf + sz;
			const uint8_t *src = buf + (sz >> 1);
			uint32_t cnt = sz >> 1;
			for (;;)
			{
				const uint8_t b = *(src - 1);
				src -= 1;
				dst -= 2;
				dst[0] = b; dst[1] = b;
				const uint32_t co = cnt; cnt -= 1;
				if (cnt == 0 || (int32_t)co <= 0) break;
			}
		}
		else                                                    // :183-195 the low byte of each pair
		{
			uint32_t cnt = bufsz >> 1;
			const uint8_t *src = buf;
			uint8_t *dst = buf;
			if (de2 & 8) cnt = bufsz >> 2;
			for (;;)
			{
				const uint8_t lo = src[0];
				src += 2;
				*dst = lo;
				const uint32_t co = cnt; cnt -= 1;
				dst += 1;
				if (cnt == 0 || (int32_t)co <= 0) break;
			}
		}
	}
}

// Coder 8 (gdv_cutscene.c:432-526), the one nearly every frame uses: a
// bitstream of four operations over the previous picture, starting at
// flags >> 8 and stopped once the write position passes the end.
void GdvDecoder::Fmt8(const uint8_t *payload, uint32_t flags)
{
	uint8_t *const base = mBuf.data() + mGuard;
	uint8_t *dst = base + (flags >> 8);
	uint8_t *const end = base + mPixelBytes;
	uint8_t *const hard = mBuf.data() + mBuf.size() - 0x400;
	Bits br;
	br.Start(payload);
	for (;;)
	{
		if (dst > end) return;                                  // :445
		if (dst > hard || br.p > mReadLimit) { mError = "coder 8 ran past the buffer"; return; }
		const uint32_t op = br.Take(2);
		if (op == 0)                                            // :447-471 literal(s)
		{
			if (br.Take(1) == 0) { *dst++ = br.Byte(); continue; }
			const uint32_t len = br.RunLength();
			dst = CopyWords(dst, br.p, len);
			br.p += len;
			continue;
		}
		if (op == 1)                                            // :472-479 skip
		{
			if (br.Take(1) == 0) { dst += br.Take(4) + 2; continue; }
			const uint32_t b = br.Byte();
			if (!(b & 0x80)) { dst += b + 0x12; continue; }
			const uint32_t lo = br.Byte();
			dst += (((b & 0x7f) << 8) | lo) + 0x92;
			continue;
		}
		if (op == 3)                                            // :480-499 12-bit back reference
		{
			const uint32_t b = br.Byte();
			uint32_t count, dist;
			if (!(b & 0x80))
			{
				count = (b >> 4) + 6;
				dist = ((b & 0xf) << 8) | br.Byte();
			}
			else if (!(b & 0x40))
			{
				count = (uint8_t)(b - 0x72);
				const uint32_t hi = br.Take(4);
				dist = (hi << 8) | br.Byte();
			}
			else                                                // a forward reference
			{
				count = (uint8_t)(b - 0xb8);
				const uint32_t hi = br.Take(4);
				dist = (hi << 8) | br.Byte();
				dst = CopyWords(dst, dst + dist + 1, count);
				continue;
			}
			if (dist == 0xfff) { dst = RepeatPrev(dst, count); continue; }
			dst = CopyWords(dst, dst + dist - 0x1000, count);
			continue;
		}
		const uint32_t op2 = br.Take(2);                        // :500-525
		if (op2 == 3)
		{
			const uint32_t b = br.Byte();
			const uint32_t cnt = 2 + (b >> 7);
			const uint32_t x = b & 0x7f;
			if (x == 0) { dst = RepeatPrev(dst, cnt); continue; }
			dst = CopyBytes(dst, dst - 1 - x, cnt);
			continue;
		}
		const uint32_t hi = br.Take(4);
		const uint32_t lo = br.Byte();
		const uint32_t dist = (hi << 8) | lo;
		if (op2 == 0 && dist > 0xf80)                           // :513-521 the end, or a repeated pair
		{
			if (lo == 0xff) return;
			const uint32_t a = dist & 0x7f;
			const uint32_t cnt = (a & 0xf) + 2;
			const int32_t off = (int32_t)~(a >> 4);
			const uint8_t w0 = dst[off], w1 = dst[off + 1];
			for (uint32_t i = 0; i < cnt; i++) { dst[0] = w0; dst[1] = w1; dst += 2; }
			continue;
		}
		const uint32_t cnt = op2 + 3;
		if (dist == 0xfff) { dst = RepeatPrev(dst, cnt); continue; }
		dst = CopyWords(dst, dst + dist - 0x1000, cnt);
	}
}

// Coder 6 (gdv_cutscene.c:535-610): coder 8 without the end check, and a
// back reference with a longer count instead of the three-way split.
void GdvDecoder::Fmt6(const uint8_t *payload, uint32_t flags)
{
	uint8_t *const base = mBuf.data() + mGuard;
	uint8_t *dst = base + (flags >> 8);
	uint8_t *const hard = mBuf.data() + mBuf.size() - 0x400;
	Bits br;
	br.Start(payload);
	for (;;)
	{
		if (dst > hard || dst < mBuf.data() + 0x100 || br.p > mReadLimit) { mError = "coder 6 ran out of the buffer"; return; }
		const uint32_t op = br.Take(2);
		if (op == 0)
		{
			if (br.Take(1) == 0) { *dst++ = br.Byte(); continue; }
			const uint32_t len = br.RunLength();
			dst = CopyWords(dst, br.p, len);
			br.p += len;
			continue;
		}
		if (op == 1)
		{
			if (br.Take(1) == 0) { dst += br.Take(4) + 2; continue; }
			const uint32_t b = br.Byte();
			if (!(b & 0x80)) { dst += b + 0x12; continue; }
			const uint32_t lo = br.Byte();
			dst += (((b & 0x7f) << 8) | lo) + 0x92;
			continue;
		}
		if (op == 2)
		{
			const uint32_t op2 = br.Take(2);
			if (op2 == 3)
			{
				const uint32_t b = br.Byte();
				const uint32_t cnt = 2 + (b >> 7);
				const uint32_t x = b & 0x7f;
				if (x == 0) { dst = RepeatPrev(dst, cnt); continue; }
				dst = CopyBytes(dst, dst - 1 - x, cnt);
				continue;
			}
			const uint32_t hi = br.Take(4);
			const uint32_t lo = br.Byte();
			const uint32_t dist = (hi << 8) | lo;
			if (op2 == 0 && dist > 0xf80)
			{
				if (lo == 0xff) return;                         // :588, the only way out
				const uint32_t a = dist & 0x7f;
				const uint32_t cnt = (a & 0xf) + 2;
				const int32_t off = (int32_t)~(a >> 4);
				const uint8_t w0 = dst[off], w1 = dst[off + 1];
				for (uint32_t i = 0; i < cnt; i++) { dst[0] = w0; dst[1] = w1; dst += 2; }
				continue;
			}
			const uint32_t cnt = op2 + 3;
			if (dist == 0xfff) { dst = RepeatPrev(dst, cnt); continue; }
			dst = CopyWords(dst, dst + dist - 0x1000, cnt);
			continue;
		}
		const uint32_t b = br.Byte();                           // :601-608
		uint32_t count = b >> 4;
		if (count == 0xf) count = br.Byte() + 0xf;
		count += 6;
		const uint32_t dist = ((b & 0xf) << 8) | br.Byte();
		if (dist == 0xfff) { dst = RepeatPrev(dst, count); continue; }
		dst = CopyWords(dst, dst + dist - 0x1000, count);
	}
}

// Coder 5 (gdv_cutscene.c:615-643): byte-oriented, four two-bit codes per
// control byte, most significant first.
void GdvDecoder::Fmt5(const uint8_t *payload, uint32_t flags)
{
	const uint8_t *p = payload;
	uint8_t *dst = mBuf.data() + mGuard + (flags >> 8);
	uint8_t *const hard = mBuf.data() + mBuf.size() - 0x400;
	for (;;)
	{
		if (dst > hard || p > mReadLimit) { mError = "coder 5 ran past the buffer"; return; }
		const uint32_t b = *p++;
		for (int k = 0; k < 4; k++)
		{
			const uint32_t code = (b >> (6 - 2 * k)) & 3;
			if (code == 0) *dst++ = *p++;
			else if (code == 1)
			{
				const uint32_t w = Rd16(p); p += 2;
				const uint32_t dist = w >> 4, cnt = (w & 0xf) + 3;
				if (dist == 0xfff) dst = RepeatPrev(dst, cnt);
				else dst = CopyWords(dst, dst + dist - 0x1000, cnt);
			}
			else if (code == 2)
			{
				const uint32_t d = *p;
				if (d == 0) return;                             // :632
				if (d == 0xff) { const uint32_t w = Rd16(p + 1); p += 3; dst += w + 1; }
				else { p++; dst += d + 1; }
			}
			else
			{
				const uint32_t d = *p++;
				const uint32_t dist = d >> 2, cnt = (d & 3) + 2;
				if (dist == 0) dst = RepeatPrev(dst, cnt);
				else dst = CopyWords(dst, dst - dist - 1, cnt);
			}
		}
	}
}

// Coder 2, 8-bit pixels (gdv_cutscene.c:649-675). It first lays the colour
// table out again as 16-byte runs when its ninth byte is not 0 -- the
// layout gdv_init_pixel_tables left -- and always starts at the top.
void GdvDecoder::Fmt2(const uint8_t *payload)
{
	uint8_t *t = mBuf.data();
	if (t[8] != 0)
		for (int i = 0; i < 256; i++) { memset(t, i, 16); t += 16; }
	const uint8_t *p = payload;
	uint8_t *dst = mBuf.data() + mGuard;
	uint8_t *const hard = mBuf.data() + mBuf.size() - 0x400;
	for (;;)
	{
		if (dst > hard || p > mReadLimit) { mError = "coder 2 ran past the buffer"; return; }
		uint32_t acc = *p++;
		for (int k = 0; k < 4; k++)
		{
			acc <<= 2;
			const uint32_t code = (acc >> 8) & 3;
			if (code == 0) *dst++ = *p++;
			else if (code == 1)
			{
				const uint32_t w = Rd16(p);
				const uint32_t dist = w >> 4, cnt = (w & 0xf) + 3;
				dst = CopyWords(dst, dst + dist - 0x1000, cnt);
				p += 2;
			}
			else if (code == 2) { const uint32_t d = *p++; dst += d + 2; }
			else return;
		}
	}
}

// Coder 2, 16-bit pixels (gdv_cutscene.c:681-708).
void GdvDecoder::Fmt2Hi(const uint8_t *payload)
{
	uint8_t *t = mBuf.data();
	if (Rd16(t + 8) != 0x20)
	{
		uint32_t v = 0;
		for (int i = 0; i < 0x400; i++)
		{
			for (int k = 0; k < 2; k++) { t[0] = (uint8_t)v; t[1] = (uint8_t)(v >> 8); t[2] = (uint8_t)(v >> 16); t[3] = (uint8_t)(v >> 24); t += 4; }
			v += 0x200020u;
		}
	}
	const uint8_t *p = payload;
	uint8_t *dst = mBuf.data() + mGuard;
	uint8_t *const hard = mBuf.data() + mBuf.size() - 0x400;
	for (;;)
	{
		if (dst > hard || p > mReadLimit) { mError = "coder 2 ran past the buffer"; return; }
		uint32_t acc = *p++;
		for (int k = 0; k < 4; k++)
		{
			acc <<= 2;
			const uint32_t code = (acc >> 8) & 3;
			if (code == 0) { dst[0] = p[0]; dst[1] = p[1]; dst += 2; p += 2; }
			else if (code == 1)
			{
				const uint32_t w = Rd16(p);
				const uint32_t dist = w >> 4, cnt = (w & 0xf) + 3;
				const uint8_t *src = dst + dist * 2 - 0x2000;
				for (uint32_t i = 0; i < cnt; i++) { dst[0] = src[0]; dst[1] = src[1]; dst += 2; src += 2; }
				p += 2;
			}
			else if (code == 2) { const uint32_t d = *p++; dst += d * 2 + 4; }
			else return;
		}
	}
}

// Coder 0, a palette (gdv_cutscene.c:715-721): into the header buffer and
// straight to the DAC.
void GdvDecoder::Fmt0(const uint8_t *payload)
{
	mDe5 = 1;
	memcpy(mHeader.data() + 0x18, payload, 0x300);
	WritePalette(mHeader.data() + 0x18, 0, 0x100);
}

// Coder 1, a palette and a clear (gdv_cutscene.c:731-758). With flag bits
// above the low byte, an 8-bit movie and stream flag 0x8000, it is a flash:
// DAC entry 255 takes the colour in those bits and the picture is cleared
// to 255, and the next chunk clears it again first. The settle countdown
// then blacks the DAC for a frame and puts the new palette up.
void GdvDecoder::Fmt1(const uint8_t *chunk, uint32_t flags)
{
	mDe5 = 1;
	const uint32_t hi = flags & 0xffffff00u;
	mDec = 0;
	uint8_t fill = 0;
	if (hi != 0 && mVersion == 1 && (mStreamFlags & 0x8000))
	{
		mDec = 0xff;
		const uint32_t rgb = (hi >> 0xa) & 0x3f3f3fu;
		mDac[0x2fd] = (uint8_t)rgb;
		mDac[0x2fe] = (uint8_t)(rgb >> 8);
		mDac[0x2ff] = (uint8_t)(rgb >> 16);
		fill = 0xff;
		mDeb = 0xff;
	}
	ClearPixels(fill);
	mDed = 0;
	mDe4 = 2;
	memcpy(mHeader.data() + 0x18, chunk + 8, 0x300);
}

//==========================================================================
//
// The DAC.
//
//==========================================================================

// write_vga_palette (video_display.c:391-433): `count` entries from `start`,
// a do-while on a signed 16-bit count, the index register wrapping at 256.
// 8-bit movies only. The cutscene's callback (the subtitle machine, mode 3)
// sees every call first.
void GdvDecoder::WritePalette(const uint8_t *src, int start, int count)
{
	if (mVersion != 1) return;
	mPaletteWrites++;
	uint16_t cx = (uint16_t)count;
	uint8_t idx = (uint8_t)start;
	for (;;)
	{
		mDac[idx * 3 + 0] = src[0];
		mDac[idx * 3 + 1] = src[1];
		mDac[idx * 3 + 2] = src[2];
		src += 3;
		idx++;
		cx = (uint16_t)(cx - 1);
		if ((int16_t)cx <= 0) break;
	}
}

// gdv_clear_vga_palette (gdv_cutscene.c:950-964): entries 0 .. [0x91dec]-1
// black, all 256 when that is 0. 8-bit movies only.
void GdvDecoder::ClearDac()
{
	if (mVersion != 1) return;
	uint8_t i = 0;
	do
	{
		mDac[i * 3 + 0] = mDac[i * 3 + 1] = mDac[i * 3 + 2] = 0;
		i++;
	} while (i != mDec);
}

// gdv_settle_palette_fade (gdv_cutscene.c:972-987).
void GdvDecoder::Settle()
{
	if (mDe4 == 0) return;
	mDe4--;
	if (mDe4 != 0) { ClearDac(); return; }
	WritePalette(mHeader.data() + 0x18, 0, mDec != 0 ? mDec : 0x100);
}

// The blitter's own state changes (traps.c / gdv_cutscene.c:1846-1856): a
// decoded palette chunk restores the header's palette copy, and the settle
// count is set to 1.
void GdvDecoder::BlitSideEffects()
{
	if (mDe5 != 0 && mVersion == 1) memcpy(mHeader.data() + 0x18, mHeader.data() + 0x318, 0x300);
	mDe5 = 0;
	mDe4 = 1;
}

// Putting a frame up: the blitter's dispatch on the chunk flags (0x4c788,
// gdv_cutscene.c:1864-1872) and the settle it ends with on every path but
// its three early returns (the jmp 0x4d2e0 the image-free lift keeps in
// traps.c host_gdv_publish_frame).
void GdvDecoder::Emit()
{
	const uint8_t al = mDc0;
	if (al & 0x80) return;
	if (al & 0x40) { BlitSideEffects(); Settle(); return; }
	if ((al & 0xf) == 0 || (al & 0xf) == 3) return;
	Settle();
}

void GdvDecoder::LoopStartPalette()
{
	WritePalette(mHeader.data() + 0x18, 0, 0x100);              // gdv_cutscene.c:2905
	mDe5 = 0;                                                   // :2906
}

//==========================================================================
//
// Audio: the decoder [0x91898] points at (os_audio.c:253-273 for DPCM).
//
//==========================================================================

void GdvDecoder::DecodeAudio(const uint8_t *rec)
{
	mAudioOut.clear();
	if (mAudioBytes == 0) return;
	const uint8_t *s = rec;
	const uint32_t count = mAudioBytes;
	if (mAudioFlags & 8)
	{
		// Two running sums, the even bytes into the first and the odd into
		// the second, restarted by the chunk's 0x40 flag (the byte at
		// count + 4, the chunk's flags), the loop a do-while.
		if (s[count + 4] & 0x40) { mSumL = 0; mSumR = 0; }
		int32_t c = (int32_t)(count >> 1);
		mAudioOut.reserve((size_t)count);
		const uint8_t *p = s;
		do
		{
			mSumL += (uint32_t)mStep[p[0]];
			mAudioOut.push_back((int16_t)(uint16_t)mSumL);
			mSumR += (uint32_t)mStep[p[1]];
			mAudioOut.push_back((int16_t)(uint16_t)mSumR);
			p += 2;
		} while (--c > 0);
	}
	else if (mAudioFlags & 4)
	{
		// 16-bit PCM: no decoder (the bare-ret entry 0x4e45f).
		for (uint32_t i = 0; i + 1 < count; i += 2) mAudioOut.push_back((int16_t)Rd16(s + i));
	}
	else
	{
		// 8-bit unsigned PCM, played as it is; widened here for the mixer.
		for (uint32_t i = 0; i < count; i++) mAudioOut.push_back((int16_t)(((int)s[i] - 0x80) << 8));
	}
}

//==========================================================================
//
// The picture, expanded.
//
//==========================================================================

void GdvDecoder::ExpandPicture(std::vector<uint8_t> &out) const
{
	out.assign((size_t)mWidth * (size_t)mHeight, 0);
	if (mVersion != 1) return;
	const uint8_t *src = Pixels();
	const int hh = (mDe2 & 4) ? 1 : 0;      // horizontal half: columns doubled
	const int vh = (mDe2 & 8) ? 1 : 0;      // vertical half: rows doubled
	const int stride = mWidth >> hh;
	for (int y = 0; y < mHeight; y++)
	{
		const uint8_t *row = src + (size_t)(y >> vh) * (size_t)stride;
		uint8_t *d = out.data() + (size_t)y * (size_t)mWidth;
		if (!hh) memcpy(d, row, (size_t)mWidth);
		else for (int x = 0; x < mWidth; x++) d[x] = row[x >> 1];
	}
}

//==========================================================================
//
// Subtitles: run_timed_message_sequence (dialogue_ui.c:1884-2008).
//
//==========================================================================

void GdvSubtitles::Start(const uint8_t *stream, size_t size)
{
	*this = GdvSubtitles();
	if (stream != nullptr && size > 0) mStream.assign(stream, stream + size);
}

bool GdvSubtitles::Frame(uint32_t sampleClock)
{
	mRedrawn = false;
	if (mStream.empty()) return false;
	// The cursor is a pointer into the loaded stream; a cursor that has run
	// off it would read whatever follows. The retail streams end with a
	// { 0, 0xffff } entry the clock never reaches.
	if (mCursor + 5 > mStream.size()) return false;
	const uint8_t *e = mStream.data() + mCursor;
	const uint32_t now = sampleClock / GDV_SUBTITLE_UNIT + 1;     // :1913-1916
	const uint32_t at = (uint32_t)(e[2] | (e[3] << 8));
	if (now < at) return false;                                  // :1918
	if (mRegion)                                                 // :1919-1921 clear first
	{
		mRegion = false;
		mShownAt = -1;
		mLast = -1;                                              // :1921
		mText.clear();
		return true;
	}
	const int16_t size = (int16_t)(e[0] | (e[1] << 8));
	bool changed = false;
	if (size > 6)                                                // :1923-1952 draw it
	{
		mColor = e[4];
		const size_t end = mCursor + (size_t)size <= mStream.size() ? mCursor + (size_t)size : mStream.size();
		mText.clear();
		for (size_t i = mCursor + 5; i < end && mStream[i] != 0; i++) mText += (char)mStream[i];
		mRegion = true;
		mRedrawn = true;
		changed = true;
	}
	mShownAt = mRegion ? (int)mCursor : -1;
	mLast = (int)mCursor;                                        // :1954 [0x83b34]
	mCursor += (uint16_t)size;                                   // :1955
	return changed;
}

void GdvSubtitles::PaletteWritten()
{
	// dialogue_ui.c:1898-1909: with an entry drawn, its rectangle is
	// forgotten (not cleared) and the cursor goes back to it, so the next
	// frame draws it again over itself in the new palette.
	if (mStream.empty() || mLast < 0) return;
	mRegion = false;
	mCursor = (size_t)mLast;
}

uint8_t GdvSubtitles::NearestIndex(const uint8_t seed[3], const uint8_t *pal)
{
	uint32_t best = 100000;
	uint8_t bestIdx = 0;
	for (int i = 0; i < 256; i++)
	{
		int dg = (int)pal[i * 3 + 1] - (int)seed[1]; if (dg < 0) dg = -dg;
		int dr = (int)pal[i * 3 + 0] - (int)seed[0]; if (dr < 0) dr = -dr;
		int db = (int)pal[i * 3 + 2] - (int)seed[2]; if (db < 0) db = -db;
		const uint32_t dist = (uint32_t)(dg * 4 + dr * 2 + db);
		if (dist <= best) { best = dist; bestIdx = (uint8_t)i; }
	}
	return bestIdx;
}

//==========================================================================
//
// Where the movie and its subtitle sit on the original's 640x480 screen.
//
//==========================================================================

GdvScreenPlacement GdvPlacementFor640x480(int width, int height)
{
	GdvScreenPlacement p;
	// init_gdv_video_context sets stream flag 0x2000 when the screen pitch
	// is 640 or more (gdv_cutscene.c:1101-1102), which doubles the picture's
	// width; gdv_init_frame_geometry centres it (:223-247): x = -(w - 640)
	// >> 1, y = -(h - 480) >> 1, both arithmetic shifts.
	p.w = width * 2;
	p.h = height;
	p.x = -(p.w - 640) >> 1;
	p.y = -(p.h - 480) >> 1;
	// The subtitle's row (dialogue_ui.c:1935-1938): half the screen height
	// plus 0x48, plus 0x48 again on a screen 301 or more rows high; the text
	// starts one row and one column in (text_font.c:304, `lea edi,[edi+ebx+1]`).
	p.subtitleY = (480 >> 1) + 0x48 + 0x48;
	return p;
}

} // namespace roth
