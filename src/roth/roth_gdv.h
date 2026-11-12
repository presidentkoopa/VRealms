#pragma once
//
// Realms of the Haunting's full-motion video: the GDV ("Gremlin Digital
// Video") decoder, its audio, its palette as the screen shows it, its pacing
// and its subtitles. Engine-free: the C++ standard library only, so
// tools/gdv_test builds it and runs it over the retail movies, and the same
// numbers are read out of the running original (tools/gdv_test/gdvprobe.c).
//
// Every rule is read from ROTH.C (roth_c/src/engine/gdv_cutscene.c and the
// files it calls) and cited as file.c:LINE. The code is written fresh; it is
// not a copy of the reconstruction.
//
// THE FORMAT, AS THE ORIGINAL READS IT
//
//   header    0x18 bytes (gdv_read_file_header, gdv_cutscene.c:1237-1333):
//             +0 magic 0x29111994, +4 size code, +6 frame count, +8 frames
//             per second, +0xa audio flags, +0xc sample rate, +0xe version
//             (1 = 8-bit pixels with a 0x300-byte palette after the header,
//             2 = 16-bit pixels), +0x10 largest chunk, +0x14 width,
//             +0x16 height (0 = look the size code up in a table).
//   record    one per frame, packed back to back: the frame's audio block
//             (a fixed size worked out from the header) and then a chunk:
//             +0 magic 0x1305, +2 payload size, +4 flags, payload
//             (gdv_read_frame_chunk, gdv_cutscene.c:1365-1418).
//   video     the chunk flags' low nibble picks the coder: 8, 6, 5, 2 are
//             LZ/RLE variants writing into the previous picture, 0 is a
//             palette, 1 a palette with a clear (and an optional flash),
//             3 changes nothing (gdv_decode_video_chunk, :760-814). Bits
//             0x10/0x20 halve the picture horizontally/vertically for that
//             frame (:781-786), 0x40 clears first (:775-779), 0x80 means the
//             frame is decoded but not put on the screen (:1864-1872), and
//             flags >> 8 is where an 8/6/5 coder starts writing (:434).
//   audio     DPCM: one byte per sample through a 256-entry step table into
//             two running sums, left and right (os_audio.c:253-273, the
//             table gdv_cutscene.c:846-862). A chunk with flag 0x40 restarts
//             both sums.
//   pacing    the sound card plays the movie's audio at 22050 Hz
//             (gdv_init_audio_output passes 0x5622, gdv_cutscene.c:2313;
//             os_audio.c:174), and one video frame is decoded per audio
//             block consumed, so a frame lasts samplesPerFrame / 22050 s.
//             Silent, the timer gives the same rate (fps << 8, +0x80 for
//             21168 Hz movies: gdv_cutscene.c:1312, 2309-2310, 2568-2590).
//
// THE PALETTE THE SCREEN SHOWS
//
//   The decoder holds the movie palette in the header buffer (+0x18) and the
//   VGA's DAC is written only at set points: the full palette when the loop
//   starts (gdv_cutscene.c:2905), at once for a palette chunk (:720), and
//   through the "settle" countdown a palette+clear chunk arms (:754): one
//   frame of black, then the new palette (:972-987, run as the blitter's
//   tail on every frame it puts up). A flash chunk paints index 255 its own
//   colour and clears to it (:738-749). Dac() is that DAC.
//
// SUBTITLES
//
//   A movie's subtitle stream is a DBASE400 range named by its DBASE100
//   cutscene entry (gdv_cutscene.c:2777-2797). Entries are { int16 size,
//   uint16 time, uint8 colour, text... }; time is tenths of a second of the
//   movie's own audio clock (decoded samples / 0x89d). run_timed_message_
//   sequence (dialogue_ui.c:1884-2008) runs once per frame put up: when the
//   clock reaches an entry, a subtitle on screen is cleared first, and on
//   the next frame the entry is shown (if it has text) and the cursor
//   moves on. GdvSubtitles is that machine.
//

#include <stdint.h>
#include <string>
#include <vector>

namespace roth
{

// The stream flags play_gdv_cutscene gives a full-screen movie
// (gdv_cutscene.c:2643): 0x48980 normally, 0x8190 when the byte at
// g_player_movement_enabled + 0x10 is set. Only bit 0x8000 (the flash of a
// palette+clear chunk, :738) reaches what this file decodes.
static const uint32_t GDV_CUTSCENE_FLAGS = 0x48980;

// The sound card rate a movie's audio is played at (gdv_cutscene.c:2313).
static const int GDV_OUTPUT_RATE = 0x5622;

// One clock unit of the subtitle timer: decoded samples / 0x89d
// (dialogue_ui.c:1913) -- a tenth of a second at 22050 Hz.
static const uint32_t GDV_SUBTITLE_UNIT = 0x89d;

class GdvDecoder
{
public:
	// gdv_read_file_header + gdv_setup_decode_buffers + gdv_init_pixel_tables
	// (gdv_cutscene.c:1237-1333, 877-931, 66-97). `data` must stay alive.
	bool Open(const uint8_t *data, size_t size, uint32_t streamFlags = GDV_CUTSCENE_FLAGS);
	const std::string &Error() const { return mError; }

	int Width() const { return mWidth; }
	int Height() const { return mHeight; }
	int FrameCount() const { return mFrameCount; }
	int Fps() const { return mFps; }
	int HeaderRate() const { return mRate; }
	int Version() const { return mVersion; }        // 1 = 8-bit pixels
	int BytesPerPixel() const { return mBpp; }

	// Audio (gdv_cutscene.c:1293-1316). Channels and bits describe what
	// AudioBlock() holds after decoding, not the file.
	bool HasAudio() const { return mAudioBytes != 0; }
	int AudioChannels() const { return (mAudioFlags & 2) ? 2 : 1; }
	bool AudioIsDpcm() const { return (mAudioFlags & 8) != 0; }
	// [0x91d58]: samples a frame stands for (sample rate / fps, a 16-bit
	// divide, gdv_cutscene.c:1295-1302); 1837 for a silent movie.
	int SamplesPerFrame() const { return (int)mSamplesPerFrame; }
	// [0x91d28]: bytes of audio stored per record.
	uint32_t AudioBytesPerRecord() const { return mAudioBytes; }

	// Off: NextFrame() reads each record and decodes only its audio (a second
	// decoder feeding the sound stream from the mixer's thread).
	void SetVideo(bool on) { mVideo = on; }

	// One record: read the next chunk, decode its picture and its audio
	// block, then do what the original does when it puts the frame up
	// (gdv_emit_decoded_frame, gdv_cutscene.c:1550-1571, and the blitter's
	// settle tail). False at the end or on a damaged record (Error()).
	bool NextFrame();
	int FrameIndex() const { return mFrameIndex; }      // the record just decoded, 0-based

	// The decode buffer: Width()*Height()*bpp bytes, rounded up to 4
	// ([0x91d94]). In a half-resolution frame only the top/left part is the
	// picture; HalfFlags() says which (bit 4 horizontal, bit 8 vertical).
	const uint8_t *Pixels() const { return mBuf.data() + mGuard; }
	uint32_t PixelBytes() const { return mPixelBytes; }
	uint8_t HalfFlags() const { return mDe2; }
	// The chunk flags' low byte ([0x91dc0]).
	uint8_t ChunkFlags() const { return mDc0; }
	// Did this frame reach the screen? The blitter returns at once for bit
	// 0x80 and for coders 0 and 3 (gdv_cutscene.c:1864-1872).
	bool FrameShown() const { return !(mDc0 & 0x80) && (mDc0 & 0xf) != 0 && (mDc0 & 0xf) != 3; }
	// Expand the buffer to a full Width() x Height() picture of palette
	// indices, undoing the half-resolution layout (the blitter doubles, as
	// traps.c's publish of the same buffer does). 8-bit movies only.
	void ExpandPicture(std::vector<uint8_t> &out) const;

	// The movie palette in the header buffer (+0x18, 6-bit VGA values) and
	// the DAC as the screen shows it after this frame (see the top).
	const uint8_t *MoviePalette() const { return mHeader.data() + 0x18; }
	const uint8_t *Dac() const { return mDac; }
	// What the loop does before the first frame's picture stays up: the
	// whole palette to the DAC (gdv_run_playback_loop, gdv_cutscene.c:2905).
	// NextFrame() calls it after record 0, so Dac() after the first frame
	// is the palette the movie opens on (the fade-in ends there).
	void LoopStartPalette();
	// The DAC writes this record made, split where the subtitle machine's
	// frame call falls between them: the cutscene's callback sees every
	// write_vga_palette first (mode 3, video_display.c:395-423), and its
	// per-frame call (mode 0) comes after the chunk is decoded and before the
	// blitter's settle tail (gdv_emit_decoded_frame, gdv_cutscene.c:1555-1566).
	// So: PaletteWritten() DecodeWrites() times, Frame(), then
	// PaletteWritten() EmitWrites() times.
	int DecodeWrites() const { return mWritesDecode; }
	int EmitWrites() const { return mWritesEmit; }

	// This record's audio, decoded: interleaved 16-bit samples for DPCM and
	// 16-bit files, unsigned 8-bit samples widened to 16 for 8-bit files.
	const std::vector<int16_t> &AudioBlock() const { return mAudioOut; }

	// [0x91d54]: decoded-sample clock, advanced by SamplesPerFrame() at the
	// start of every chunk (gdv_cutscene.c:763). The subtitle timer reads it.
	uint32_t SampleClock() const { return mClock; }

	// The video decode alone, for a caller holding its own chunk bytes
	// (gdv_decode_video_chunk, gdv_cutscene.c:760-814). `chunk` points at
	// the 0x1305 magic.
	bool DecodeChunk(const uint8_t *chunk);

private:
	bool Fail(const char *what) { mError = what; return false; }
	void Reformat();
	void Fmt8(const uint8_t *p, uint32_t flags);
	void Fmt6(const uint8_t *p, uint32_t flags);
	void Fmt5(const uint8_t *p, uint32_t flags);
	void Fmt2(const uint8_t *p);
	void Fmt2Hi(const uint8_t *p);
	void Fmt0(const uint8_t *p);
	void Fmt1(const uint8_t *chunk, uint32_t flags);
	void ClearPixels(uint8_t value);
	void WritePalette(const uint8_t *src, int start, int count);
	void ClearDac();
	void Settle();
	void BlitSideEffects();
	void Emit();
	void DecodeAudio(const uint8_t *rec);

	std::string mError;
	const uint8_t *mData = nullptr;
	size_t mSize = 0;
	size_t mPos = 0;
	uint32_t mStreamFlags = 0;

	std::vector<uint8_t> mHeader;           // [0x91d44]: 0x18 header + palette + its backup copy
	int mWidth = 0, mHeight = 0, mFrameCount = 0, mFps = 0, mRate = 0;
	int mVersion = 0, mBpp = 1;
	uint16_t mAudioFlags = 0;
	uint32_t mSamplesPerFrame = 0;          // [0x91d58]
	uint32_t mAudioBytes = 0;               // [0x91d28]
	uint32_t mMaxChunk = 0;                 // header +0x10
	uint32_t mPixelBytes = 0;               // [0x91d94]
	uint32_t mGuard = 0;                    // [0x91cc4]: the LUT before the picture

	std::vector<uint8_t> mBuf;              // [0x91d3c] LUT, then [0x91d40] the picture, then slack
	std::vector<uint8_t> mScratch;          // the chunk payload being decoded, zeros after it
	const uint8_t *mReadLimit = nullptr;
	int mFrameIndex = -1;
	uint32_t mClock = 0;                    // [0x91d54]
	uint8_t mDc0 = 0;                       // [0x91dc0]
	uint8_t mDe2 = 0, mDe3 = 0;             // [0x91de2] half-res bits, [0x91de3] the previous
	uint8_t mDe4 = 0;                       // [0x91de4] the settle countdown
	uint8_t mDe5 = 0;                       // [0x91de5] a palette chunk was decoded
	uint8_t mDeb = 0;                       // [0x91deb] clear before the next chunk
	uint8_t mDec = 0;                       // [0x91dec] DAC entries the settle handles (0 = 256)
	uint8_t mDed = 0;                       // [0x91ded] the buffer holds a picture
	uint8_t mDac[0x300] = {};
	bool mVideo = true;
	int mPaletteWrites = 0;
	int mWritesDecode = 0, mWritesEmit = 0;

	int32_t mStep[256] = {};                // [0x918a4], gdv_build_scale_table
	uint32_t mSumL = 0, mSumR = 0;          // [0x9189c], [0x918a0]
	std::vector<int16_t> mAudioOut;
};

// The subtitle machine (run_timed_message_sequence modes 0 and 3,
// dialogue_ui.c:1884-2008), without the pixels.
class GdvSubtitles
{
public:
	// `stream` is the DBASE400 range the cutscene entry names (offset
	// +0x10, size +0x0a; gdv_cutscene.c:2777-2797). Empty = no subtitles.
	void Start(const uint8_t *stream, size_t size);
	bool Active() const { return !mStream.empty(); }

	// Mode 0, once per frame put up, with the decoder's SampleClock().
	// Returns true when what is on screen changed.
	bool Frame(uint32_t sampleClock);
	// Mode 3, for every DAC write: the colour cache is dropped and the
	// subtitle on screen is drawn again with the colour remapped
	// (dialogue_ui.c:1898-1909).
	void PaletteWritten();

	// What is on screen: the entry's text (NUL-free) and its colour byte,
	// an index into the GAME palette (g_palette_rgb_ptr) that the original
	// maps to the nearest movie palette entry (Showing() false = nothing).
	bool Showing() const { return mShownAt >= 0; }
	const std::string &Text() const { return mText; }
	uint8_t ColorIndex() const { return mColor; }
	// True on the frame the entry was (re)drawn, i.e. its colour must be
	// looked up again with the palette the DAC holds now.
	bool Redrawn() const { return mRedrawn; }

	// find_nearest_palette_index (video_display.c:209-226): the movie
	// palette entry nearest the game palette's colour `seed` (6-bit RGB),
	// weighting green 4, red 2, blue 1; ties go to the later entry.
	static uint8_t NearestIndex(const uint8_t seed[3], const uint8_t *moviePalette);

private:
	std::vector<uint8_t> mStream;
	size_t mCursor = 0;         // [0x83b30] the next entry
	int mShownAt = -1;          // the entry whose text is up, -1 none
	int mLast = -1;             // [0x83b34] the entry last handled, -1 (0) none
	bool mRegion = false;       // [0x83b44] != 0: a subtitle rectangle is up
	std::string mText;
	uint8_t mColor = 0;
	bool mRedrawn = false;
};

// The movie's display in the original's 640x480 mode (VideoMode 8, the
// retail ROTH.INI): a 320-wide movie is doubled horizontally and the
// picture is centred vertically (gdv_init_frame_geometry,
// gdv_cutscene.c:213-375; traps.c measures 640x280 at y 100 for the intro).
struct GdvScreenPlacement
{
	int x = 0, y = 0, w = 0, h = 0;     // in the 640x480 screen
	int subtitleY = 0;                  // the subtitle's top row (dialogue_ui.c:1935-1938)
};
GdvScreenPlacement GdvPlacementFor640x480(int width, int height);

} // namespace roth
