#pragma once
//
// Realms of the Haunting's own sound: the readers and the rules, engine-free.
//
// Everything here is read out of ROTH.C (engine/audio.c unless a citation
// says otherwise) and none of it touches the engine, so the host tool in
// tools/roth_sound compiles the same code and checks it over the retail data.
// The engine's side -- handing the samples to the sound system and playing
// them at a place in the map -- is roth_sound.cpp.
//
// NOTHING HERE WRITES A FILE. Samples, songs and speech are read from the
// player's install into memory and stay there.
//
//   SfxBank      the digitised effects: FXSCRIPT.SFX (ROTH.RES `snd=`)
//   SfxSection   a map's sound nodes and zones: RAW section 7
//   SfxVolume    distance, sample volume and zone -> a voice volume
//   SfxNodePump  which nodes play this tick: modes, timers, the 32 nearest
//   Dbase300     the songs (HMP), by chunk index
//   Dbase500     the speech clips (DPCM), by clip index
//

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace roth
{

//==========================================================================
//
// THE EFFECT BANK -- load_sound_effect_bank, audio.c:1059-1094.
//
// Header, 0x1c bytes: +0x00 the dword 0x53465830 ("0XFS" on disk),
// +0x08 the file offset of the sample table, +0x10 the table's size in BYTES.
// The row count is that size / 12 (audio.c:1085).
//
// MEASURED on the retail banks: the table follows the header directly (offset
// 0x1c), 320 rows; flags are 0x03 (16-bit 22050 Hz), 0x01 (16-bit 11025 Hz)
// and 0x00 on 19 rows that are all the same 218-byte 8-bit near-silent
// placeholder. Keys run 1..320 in row order, so key == row + 1 throughout.
//
// A table row, 12 bytes (load_sound_sample, audio.c:443-503):
//   +0x00 u32  file offset of the sample data
//   +0x04 u32  byte size; 0 or more than 0x100000 does not load (:455)
//   +0x08 u16  the KEY that DBASE100 names a sound by
//              (find_sound_sample_index, renderer.c:11408-11418)
//   +0x0a u8   flags, see below
//   +0x0b u8   runtime: which cache slot holds it; 0xff at load (:1088-1092)
//
// FLAGS, from how start_sound_voice_vol turns them into an HMI SOS voice
// (audio.c:660-689):
//   bit 1 (0x02)  a 22050 Hz sample. Clear is 11025 Hz. The original states
//                 it as a pitch: a 22 kHz sample gets rate 0x20000 (x2) when
//                 the driver is not running at 0x5622 = 22050, and an 11 kHz
//                 one gets 0x8000 (x0.5) when it is (:676-680).
//   bit 0 (0x01)  sets voice flag 0x80, else 0x20 (:674-675). In the SOS
//                 those two bits are _TRANSLATE16TO8 and _TRANSLATE8TO16,
//                 i.e. "this sample is 16-bit" / "this sample is 8-bit".
//                 [inferred from the HMI SOS flag names; ROTH.C's host mixer
//                 only ever treats samples as 16-bit mono (platform/audio.c:
//                 162-163) and docs/GAME_audio_save_menu.md called this bit
//                 "loop", which the callback at audio.c:1951-1975 disproves:
//                 looping is decided there by the NODE's mode, never by the
//                 sample.] The retail rows with the bit clear are the 19
//                 placeholders, whose bytes sit at 0x7e +-5: unsigned 8-bit
//                 silence, which fits.
//   bit 3 (0x08)  with bit 1, stops the load-time 2x interpolation
//                 (:457-459). Nothing else reads it.
//
// The original doubles 11 kHz samples to 22 kHz by linear interpolation as it
// loads them (interpolate_words_43dd8, renderer.c:9961). That is a mixer
// convenience, not content: the samples go to the engine at their own rate
// and its resampler does that job.
//
// WHICH ID IS WHICH. RAW map records and sound nodes name a sample by its
// ROW INDEX (play sites pass id - 1 where the stored id is one higher and 0
// means silent, e.g. raw_commands.c:1295; a node's +4 is the row itself,
// audio.c:887). DBASE100 names a sample by KEY in the ITEM records only, and
// those are rewritten to row index + 1 once at startup
// (resolve_dbase100_sound_ids, audio.c:1898-1924) -- see ResolveDbase100.
//
//==========================================================================

struct SfxRow
{
	uint32_t offset = 0;
	uint32_t size = 0;
	uint16_t key = 0;
	uint8_t  flags = 0;

	bool Loadable() const { return size != 0 && size <= 0x100000u; }   // audio.c:455
	int  Rate() const { return (flags & 2) ? 22050 : 11025; }
	bool Is16Bit() const { return (flags & 1) != 0; }
	// Whole samples in the row, and its length in 70 Hz ticks rounded up.
	uint32_t Frames() const { return Is16Bit() ? size / 2 : size; }
	uint32_t Ticks70() const { return (uint32_t)(((uint64_t)Frames() * 70 + Rate() - 1) / Rate()); }
};

class SfxBank
{
public:
	// Reads the header and the table. False, with Error() set, when the file
	// is missing or is not a bank; the sample data stays on disk until asked for.
	bool Open(const std::string &path);
	void Close();

	bool IsOpen() const { return mOpen; }
	const std::string &Path() const { return mPath; }
	const std::string &Error() const { return mError; }
	int Count() const { return (int)mRows.size(); }
	const SfxRow *Row(int index) const
	{ return (index >= 0 && index < (int)mRows.size()) ? &mRows[index] : nullptr; }

	// find_sound_sample_index: the 1-based row whose key matches, else 0.
	int IndexForKey(uint16_t key) const;

	// The raw bytes of one row, read from the file now. Empty on any failure.
	std::vector<uint8_t> Read(int index) const;

	// The row as a complete in-memory RIFF/WAVE image (mono, the row's own
	// rate and width). 8-bit rows are passed through as unsigned, which is
	// what WAVE's 8-bit PCM is. Empty on any failure. Never written anywhere.
	std::vector<uint8_t> ReadAsWave(int index) const;

	// resolve_dbase100_sound_ids (audio.c:1898-1924), on a DBASE100 image held
	// in memory: for every inventory record, walk its blocks and replace the
	// low word of each sound opcode {0x19,0x21,0x29,0x2a,0x31,0x32} with the
	// 1-based row of that key (0 when no row has it). Returns how many words
	// were rewritten. MUST RUN EXACTLY ONCE PER IMAGE: a second pass would
	// read row numbers as keys.
	int ResolveDbase100(uint8_t *image, size_t size) const;

private:
	bool mOpen = false;
	std::string mPath, mError;
	std::vector<SfxRow> mRows;
};

// PCM in memory -> a RIFF/WAVE image in memory.
std::vector<uint8_t> MakeWave(const uint8_t *pcm, size_t bytes, int rate, int bits);

//==========================================================================
//
// SOUND NODES AND ZONES -- RAW section 7.
//
// The section sits after the command section (map_load.c:466-479 reads it
// into g_sfx_nodes 0x85c44; in the file it starts at verticesOffset +
// verticesSectionSize + commandSectionSize and is section7Size long).
//
//   +0 u16  the offset of the zone list from the section's start
//           ([0x85c48] = base + word[base], map_load.c:476-479)
//   +2 u16  node count
//   +4      the nodes, 0x12 bytes each
//
// MEASURED over the 44 retail maps (tools/roth_sound): 1,627 nodes and 128
// zones, and in every map 4 + count * 0x12 equals the zone offset and the
// remainder is a whole number of 0x20-byte zones.
//
// A node:
//   +0x00 s16 x, +0x02 s16 y   map units (audio.c:635-636)
//   +0x04 u16  sample ROW (audio.c:887)
//   +0x06 u16  the key RAW commands find it by (audio.c:166, renderer.c:10897)
//   +0x08 u8   bit 7 = active; bits 0-2 = mode (audio.c:616-618)
//   +0x09 u8   zone, 1-based; 0 = none (audio.c:84-86, 870)
//   +0x0a u16  range (audio.c:68, 638)
//   +0x0c u16  period, modes 2 and up (audio.c:624)
//   +0x0e u16  the countdown, runtime (renderer.c:10136-10150)
//   +0x10 u8   volume, 0x40 = unity (audio.c:82-83)
//   +0x11 u8   not read by audio.c. 0 on 1,294 retail nodes, 0x80 on 333.
//
// A zone, 0x20 bytes: s16 rect count, then that many 10-byte rects
// { u8 dampen, u8 flags, s16 xLo, s16 yLo, s16 xHi, s16 yHi }, RELATIVE TO
// THE EMITTER (audio.c:86-101).
//
//==========================================================================

struct SfxNode
{
	int16_t  x = 0, y = 0;
	uint16_t sound = 0;
	uint16_t key = 0;
	uint8_t  flags = 0;       // bit 7 active, bits 0-2 mode
	uint8_t  zone = 0;
	uint16_t range = 0;
	uint16_t period = 0;
	uint16_t timer = 0;
	uint8_t  volume = 0;
	uint8_t  unk11 = 0;

	bool Active() const { return (flags & 0x80) != 0; }
	int  Mode() const { return flags & 7; }
};

struct SfxZoneRect
{
	uint8_t dampen = 0, flags = 0;
	int16_t xLo = 0, yLo = 0, xHi = 0, yHi = 0;
};

struct SfxZone
{
	int count = 0;                // as stored; the scan runs this many rects
	SfxZoneRect rects[3];         // 2 + 3 * 10 = 0x20: three is all that fits
};

struct SfxSection
{
	std::vector<SfxNode> nodes;
	std::vector<SfxZone> zones;
	std::string error;            // empty when the layout checked out

	// `bytes` is the section verbatim. An empty section is valid and empty.
	bool Parse(const uint8_t *bytes, size_t size);

	// setup_sfx_nodes (renderer.c:10136-10150): prime the countdown of every
	// ACTIVE node of mode 2 or more to low16(7 * period) >> 1.
	void Setup();

	// find_sfx_node_by_key (renderer.c:10889-10901): the FIRST node with that
	// key, or -1.
	int FindByKey(uint16_t key) const;

	const SfxZone *Zone(uint8_t oneBased) const
	{ return (oneBased >= 1 && oneBased <= zones.size()) ? &zones[oneBased - 1] : nullptr; }
};

//==========================================================================
//
// THE VOLUME LAW -- compute_sound_volume_pan, audio.c:65-114.
//
//   d   = isqrt(dist2)
//   v   = 0x7fff - d * 0x7fff / range            negative -> 0, and stop
//   v   = (v * vol8) >> 6                        unless vol8 is 0x40
//   zone: rel = player - emitter; the FIRST rect that "hits" takes
//         dampen << 7 off and ends the scan; a hit is OUTSIDE any bound for
//         a plain rect and STRICTLY INSIDE for one with flag bit 0
//   v   = (v * master) >> 15, unsigned, clamped to 0x7fff
//
// A range of 0 is silence: the original would divide by zero, and ROTH.C's
// own standalone build returns 0 there (audio.c:72-80).
//
// The square root is the original's table-driven isqrt_fixed_wrapper_3bfd6;
// this uses an exact integer floor square root. [deviation: ROTH.C's own
// tracer calls the two "within a few units" (platform/audio.c:845-847); the
// table itself was not transcribed.]
//
// `distOut`, when not null, receives d: the callers keep it (rec+0xc).
//
//==========================================================================

uint32_t SfxIsqrt(uint32_t v);

int32_t SfxVolume(int32_t range, uint8_t vol8, const SfxZone *zone,
	int32_t emitterX, int32_t emitterY, int32_t playerX, int32_t playerY,
	uint32_t dist2, uint32_t master, uint32_t *distOut);

// clamp_diff_200 (renderer.c:11296): move `center` toward `value` by at most
// 0x200. update_active_sounds slews every positional voice with it.
int32_t SfxClampDiff200(int32_t value, int32_t center);

// The starting volume of a mode-1 (looping) node: clamp_symmetric_26f2d
// (vol, 0, max(0x7ff - range, 0x10) << 4), audio.c:877-881. A loop therefore
// starts quiet and update_active_sounds fades it up.
int32_t SfxLoopStartVolume(int32_t vol, uint16_t range);

//==========================================================================
//
// THE NODE PUMP -- query_sfx_emitters_in_range, audio.c:605-648, and
// sort_sfx_query_by_distance, :181-208.
//
// Once per player tick (play_nearby_sfx_emitters, audio.c:940-950, from
// player.c:420): walk the nodes in order; skip the inactive; modes 2 and up
// count their timer down by the frame scale and are skipped until it
// expires, then reload it; whatever is left and lies within its own range of
// the player is collected, 32 at most, and sorted nearest first.
//
// `rng16` is the word at 0x85328, which mode 3 and up scale the period by:
// a 16-bit LCG stepped by tick_ambient_render_animation every 7 ticks
// (renderer.c:13413-13417, x = x * 0x5e5 + 0x29). The caller owns it.
//
// Returns indices into section.nodes, nearest first, with their dist^2.
//
//==========================================================================

struct SfxQueryEntry { int node; uint32_t dist2; };

std::vector<SfxQueryEntry> SfxNodePump(SfxSection &section, int32_t playerX, int32_t playerY,
	uint16_t frameScale, uint16_t rng16);

inline uint16_t SfxRngStep(uint16_t x) { return (uint16_t)((uint16_t)(x * 0x5e5u) + 0x29u); }

//==========================================================================
//
// THE SONGS -- load_dbase300_chunk, file_config.c:596-625.
//
// A chunk INDEX is a byte offset / 8: at index * 8 sits a u32 size, then that
// many bytes. Anything over 0x11800 is refused (:617). A song chunk is an HMI
// "HMIMIDIP" file (docs/GAME_audio_save_menu.md 2.6), which ZMusic reads.
//
//==========================================================================

// Empty on any failure, with `why` set when not null.
std::vector<uint8_t> ReadDbase300Chunk(const std::string &path, uint32_t index, std::string *why);

//==========================================================================
//
// THE SPEECH -- prime_voice_clip, audio.c:1208-1292, decode_voice_block,
// :1126-1146, decode_dpcm_block, renderer.c:9947-9956, and
// build_dpcm_step_table, renderer.c:10115-10132.
//
// A clip INDEX is a byte offset / 8 into DBASE500.DAT. There: a 0x2c-byte
// header, +0x14 u16 tag (0x2a, '*', means DPCM), +0x18 a dword the original
// stores and never reads, +0x20 s16 divisor, +0x28 the payload's byte size;
// then the payload. DPCM is one byte per sample: predictor += table[byte],
// the sample is the predictor's low 16 bits, the predictor starts at 0 and
// carries through the whole clip. Otherwise the payload is 16-bit PCM as it
// stands (the voice is submitted with flag 0x80, audio.c:1277).
//
// THE HEADER IS A WAVE HEADER, which ROTH.C does not say and the data does:
// MEASURED on the retail DBASE500.DAT, 1,467 of the clips DBASE400 names start
// "FFIR" <size> "WAVEfmt " 16, format tag 0x2a, 1 channel, 22050 Hz, block
// align 1, 8 bits, "data" <size>. So +0x14 is the format tag, +0x18 the SAMPLE
// RATE (22050 on every clip), +0x20 the block align (the "divisor") and +0x28
// the data size. The original never reads the rate: its speech voice carries
// no rate override and plays at the driver's own 22050 (the global the
// reconstruction calls "voice_sample_rate" is the speech VOLUME,
// audio.c:1867-1879, 1283). Here the header's rate is used when it is a
// plausible one, else 22050.
//
//==========================================================================

struct VoiceClip
{
	bool ok = false;
	std::string error;
	uint16_t tag = 0;          // +0x14
	uint32_t field18 = 0;      // +0x18
	int16_t  divisor = 0;      // +0x20
	uint32_t rawSize = 0;      // +0x28
	bool dpcm = false;
	int rate = 22050;
	std::vector<int16_t> pcm;  // decoded, mono
};

void BuildDpcmStepTable(int32_t table[256]);
VoiceClip ReadDbase500Clip(const std::string &path, uint32_t clip, bool decode);

//==========================================================================
//
// WHERE THE FILES ARE.
//
// `installPath` is the folder with ROTH.RES and M\ in it (the CD's data, a
// "DATA" folder on the installs seen). The game itself is started from the
// folder the installer made (CONFIG.INI's DestinationPath, a sibling "ROTH"
// folder), and ROTH.C builds its paths from CONFIG.INI
// (file_config.c:655-669):
//
//   the effect bank is  <data dir>\data\fxscript.sfx  (ROTH.RES `snd=`;
//   load_sfx_file_wrapper, audio.c:1106-1112), where <data dir> is the
//   SourcePath when Installation begins "MI" (a Minimum install plays off
//   the CD) and the DestinationPath otherwise (file_config.c:665-667).
//
// The two are DIFFERENT FILES. MEASURED on the retail set: the CD's
// DATA\FXSCRIPT.SFX is 11,462,660 bytes, 320 rows, 132 of them 11025 Hz;
// the CD's DATA\FX22.SFX is 14,764,542 bytes, the same 320 rows and keys
// with all but two of those at 22050 Hz; and a Maximum install's
// ROTH\DATA\FXSCRIPT.SFX is byte-identical to FX22.SFX. So the installer
// copies FX22.SFX in under the name the engine opens, and "FX22.SFX" is never
// a name the engine asks for (no such string in ROTH.C).
//
// FindEffectBank applies that rule, then falls back through the places the
// file has been seen, ending with FX22.SFX itself (the same bytes a Maximum
// install would have had). `how`, when not null, says which rule answered;
// `tried` collects every path that was looked at.
//
// FindRetailFile is the plain search used for DBASE300.DAT and DBASE500.DAT.
//
//==========================================================================

std::string FindRetailFile(const std::string &installPath, const char *name, std::string *tried);
std::string FindEffectBank(const std::string &installPath, std::string *how, std::string *tried);

// A stable 32-bit hash for reports (FNV-1a). The host tool prints these in
// place of the audio it must not write out.
uint32_t SfxHash(const uint8_t *data, size_t size);

} // namespace roth
