//
// Realms of the Haunting's own sound: readers and rules. See roth_sfx.h.
//
// Engine-free on purpose. Every rule carries the ROTH.C line it was read from.
//

#include "roth_sfx.h"

#include <string.h>
#include <ctype.h>
#include <algorithm>

namespace roth
{

namespace
{

inline uint16_t Rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t Rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void Wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void Wr32(std::vector<uint8_t> &o, uint32_t v)
{ o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 24)); }
inline void Wr16v(std::vector<uint8_t> &o, uint16_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); }

// Read `n` bytes at `offset`; false on a short read.
bool ReadAt(FILE *f, uint64_t offset, uint8_t *dest, size_t n)
{
	if (fseek(f, (long)offset, SEEK_SET) != 0) return false;
	return fread(dest, 1, n, f) == n;
}

long FileSize(FILE *f)
{
	if (fseek(f, 0, SEEK_END) != 0) return -1;
	return ftell(f);
}

} // namespace

//==========================================================================
//
// Where the files are
//
//==========================================================================

namespace
{

bool FileExists(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) return false;
	fclose(f);
	return true;
}

std::string UpperCopy(std::string s) { for (auto &c : s) c = (char)toupper((unsigned char)c); return s; }
std::string LowerCopy(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }

// `rel` is a relative path in upper case with forward slashes. Tried as
// given, all lower case, and with only the file name lowered.
std::string TryPath(const std::string &root, const std::string &rel, std::string *tried)
{
	const size_t slash = rel.find_last_of('/');
	const std::string dir = slash == std::string::npos ? std::string() : rel.substr(0, slash + 1);
	const std::string file = slash == std::string::npos ? rel : rel.substr(slash + 1);
	const std::string forms[] = { rel, LowerCopy(rel), dir + LowerCopy(file), LowerCopy(dir) + file };
	for (const std::string &f : forms)
	{
		const std::string path = root + "/" + f;
		if (FileExists(path)) return path;
		if (tried != nullptr) { *tried += "\n    "; *tried += path; }
	}
	return "";
}

std::string ReadText(const std::string &path)
{
	std::string out;
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) return out;
	char buf[1024];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 65536) out.append(buf, n);
	fclose(f);
	return out;
}

// The value of `key=` in an INI-style text, trimmed; empty when absent.
std::string IniValue(const std::string &text, const char *key)
{
	const std::string low = LowerCopy(text);
	const std::string want = LowerCopy(key) + "=";
	size_t at = 0;
	while ((at = low.find(want, at)) != std::string::npos)
	{
		if (at == 0 || low[at - 1] == '\n' || low[at - 1] == '\r')
		{
			size_t b = at + want.size(), e = b;
			while (e < text.size() && text[e] != '\r' && text[e] != '\n') e++;
			while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t')) e--;
			return text.substr(b, e - b);
		}
		at += want.size();
	}
	return "";
}

} // namespace

std::string FindRetailFile(const std::string &installPath, const char *name, std::string *tried)
{
	static const char *const dirs[] =
	{
		"", "DATA/", "../ROTH/", "../ROTH/DATA/", "../", "../DATA/", "ROTH/", "ROTH/DATA/",
		"GDV/", "../GDV/", "../ROTH/GDV/", "DATA/GDV/",
	};
	const std::string upper = UpperCopy(name);
	for (const char *d : dirs)
	{
		const std::string found = TryPath(installPath, std::string(d) + upper, tried);
		if (!found.empty()) return found;
	}
	return "";
}

std::string FindEffectBank(const std::string &installPath, std::string *how, std::string *tried)
{
	// The bank's name, from ROTH.RES (`snd=data\fxscript.sfx`).
	std::string rel = "DATA/FXSCRIPT.SFX";
	{
		const std::string res = TryPath(installPath, "ROTH.RES", nullptr);
		std::string snd = res.empty() ? std::string() : IniValue(ReadText(res), "snd");
		if (!snd.empty())
		{
			for (auto &c : snd) if (c == '\\') c = '/';
			rel = UpperCopy(snd);
		}
	}

	// CONFIG.INI lives in the folder the game is started from.
	static const char *const gameDirs[] = { "../ROTH", "ROTH", ".", ".." };
	for (const char *gd : gameDirs)
	{
		const std::string cfg = TryPath(installPath, std::string(gd) + "/CONFIG.INI", nullptr);
		if (cfg.empty()) continue;
		const std::string inst = UpperCopy(IniValue(ReadText(cfg), "Installation"));
		const bool minimum = inst.size() >= 2 && inst[0] == 'M' && inst[1] == 'I';      // file_config.c:656, 665
		// SourcePath is the CD's data, which is where installPath points;
		// DestinationPath is the folder CONFIG.INI is in.
		const std::string root = minimum ? installPath : installPath + "/" + gd;
		const std::string found = TryPath(root, rel, tried);
		if (!found.empty())
		{
			if (how != nullptr)
				*how = std::string("CONFIG.INI Installation=") + inst + (minimum
					? ": a Minimum install plays the bank off the CD (SourcePath)"
					: ": the bank the installer put in the game folder (DestinationPath)");
			return found;
		}
	}

	// No CONFIG.INI, or the file it points at is missing. An installed copy
	// first, then the CD's.
	static const char *const roots[] = { "../ROTH", "ROTH", ".", ".." };
	for (const char *r : roots)
	{
		const std::string found = TryPath(installPath + "/" + r, rel, tried);
		if (!found.empty())
		{
			if (how != nullptr) *how = "no usable CONFIG.INI; first copy found";
			return found;
		}
	}
	const std::string fx22 = TryPath(installPath, "DATA/FX22.SFX", tried);
	if (!fx22.empty() && how != nullptr)
		*how = "FX22.SFX from the CD: the file a Maximum install copies in as FXSCRIPT.SFX";
	return fx22;
}

uint32_t SfxHash(const uint8_t *data, size_t size)
{
	uint32_t h = 0x811c9dc5u;
	for (size_t i = 0; i < size; i++) { h ^= data[i]; h *= 0x01000193u; }
	return h;
}

//==========================================================================
//
// The bank
//
//==========================================================================

void SfxBank::Close()
{
	mOpen = false;
	mPath.clear();
	mRows.clear();
}

bool SfxBank::Open(const std::string &path)
{
	Close();
	mError.clear();
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) { mError = "cannot open " + path; return false; }

	const long fileSize = FileSize(f);
	uint8_t hdr[0x1c];
	if (fileSize < (long)sizeof(hdr) || !ReadAt(f, 0, hdr, sizeof(hdr)))
	{ fclose(f); mError = "shorter than the 0x1c-byte header"; return false; }

	if (Rd32(hdr) != 0x53465830u)                       // audio.c:1073
	{ fclose(f); mError = "no 0XFS signature"; return false; }

	const uint32_t tableOffset = Rd32(hdr + 8);          // :1086
	const uint32_t tableBytes  = Rd32(hdr + 0x10);       // :1077
	const uint32_t count = tableBytes / 12u;             // :1085
	if ((uint64_t)tableOffset + (uint64_t)count * 12u > (uint64_t)fileSize)
	{ fclose(f); mError = "sample table runs past the end of the file"; return false; }

	std::vector<uint8_t> table((size_t)count * 12u);
	if (count != 0 && !ReadAt(f, tableOffset, table.data(), table.size()))
	{ fclose(f); mError = "sample table unreadable"; return false; }
	fclose(f);

	mRows.resize(count);
	for (uint32_t i = 0; i < count; i++)
	{
		const uint8_t *r = table.data() + (size_t)i * 12u;
		mRows[i].offset = Rd32(r);
		mRows[i].size   = Rd32(r + 4);
		mRows[i].key    = Rd16(r + 8);
		mRows[i].flags  = r[0xa];
	}
	mPath = path;
	mOpen = true;
	return true;
}

int SfxBank::IndexForKey(uint16_t key) const
{
	// renderer.c:11408-11418: the first match, 1-based; 0 when there is none.
	for (size_t i = 0; i < mRows.size(); i++)
		if (mRows[i].key == key) return (int)i + 1;
	return 0;
}

std::vector<uint8_t> SfxBank::Read(int index) const
{
	std::vector<uint8_t> out;
	const SfxRow *row = Row(index);
	if (!mOpen || row == nullptr || !row->Loadable()) return out;
	FILE *f = fopen(mPath.c_str(), "rb");
	if (f == nullptr) return out;
	out.resize(row->size);
	if (!ReadAt(f, row->offset, out.data(), out.size())) out.clear();   // :480-487
	fclose(f);
	return out;
}

std::vector<uint8_t> MakeWave(const uint8_t *pcm, size_t bytes, int rate, int bits)
{
	std::vector<uint8_t> w;
	const uint16_t blockAlign = (uint16_t)(bits / 8);
	bytes -= bytes % blockAlign;
	w.reserve(44 + bytes);
	w.insert(w.end(), { 'R', 'I', 'F', 'F' });
	Wr32(w, (uint32_t)(36 + bytes));
	w.insert(w.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
	Wr32(w, 16);
	Wr16v(w, 1);                       // PCM
	Wr16v(w, 1);                       // mono
	Wr32(w, (uint32_t)rate);
	Wr32(w, (uint32_t)rate * blockAlign);
	Wr16v(w, blockAlign);
	Wr16v(w, (uint16_t)bits);
	w.insert(w.end(), { 'd', 'a', 't', 'a' });
	Wr32(w, (uint32_t)bytes);
	w.insert(w.end(), pcm, pcm + bytes);
	return w;
}

std::vector<uint8_t> SfxBank::ReadAsWave(int index) const
{
	const SfxRow *row = Row(index);
	const std::vector<uint8_t> raw = Read(index);
	if (row == nullptr || raw.empty()) return std::vector<uint8_t>();
	return MakeWave(raw.data(), raw.size(), row->Rate(), row->Is16Bit() ? 16 : 8);
}

int SfxBank::ResolveDbase100(uint8_t *image, size_t size) const
{
	// audio.c:1898-1924. The inventory table is the u32 array the header's
	// +0x14 points at, +0x10 entries long; each non-zero entry is the offset
	// of a record whose blocks start 0x14 in. A block is a header dword whose
	// low word is its byte length including itself, then (length >> 2) - 1
	// command dwords; a zero header ends the record.
	if (image == nullptr || size < 0x18) return 0;
	const uint32_t count = Rd32(image + 0x10);
	const uint32_t table = Rd32(image + 0x14);
	int rewritten = 0;
	for (uint32_t i = 0; i < count; i++)
	{
		const uint64_t slot = (uint64_t)table + (uint64_t)i * 4u;
		if (slot + 4 > size) break;
		const uint32_t rel = Rd32(image + slot);
		if (rel == 0) continue;
		uint64_t p = (uint64_t)rel + 0x14;
		for (;;)
		{
			if (p + 4 > size) break;                     // the original has no bound; a file does
			const uint32_t hdr = Rd32(image + p);
			p += 4;
			if (hdr == 0) break;
			const uint32_t n = ((hdr & 0xffff) >> 2) - 1;        // :1913, unsigned as written
			if (n > 0x4000) break;                       // a zero-length header would wrap to 2^32 - 1
			for (uint32_t j = 0; j < n; j++)
			{
				if (p + 4 > size) break;
				const uint32_t w = Rd32(image + p);
				const uint32_t op = (w >> 24) & 0x7f;
				if (op == 0x19 || op == 0x21 || op == 0x29 || op == 0x2a || op == 0x31 || op == 0x32)
				{
					Wr16(image + p, (uint16_t)IndexForKey((uint16_t)w));    // :1920
					rewritten++;
				}
				p += 4;
			}
		}
	}
	return rewritten;
}

//==========================================================================
//
// Nodes and zones
//
//==========================================================================

bool SfxSection::Parse(const uint8_t *bytes, size_t size)
{
	nodes.clear();
	zones.clear();
	error.clear();
	if (bytes == nullptr || size == 0) return true;
	if (size < 4) { error = "section shorter than its 4-byte header"; return false; }

	const uint16_t zoneOffset = Rd16(bytes);
	const uint16_t count = Rd16(bytes + 2);
	if (4u + (size_t)count * 0x12u > size) { error = "node list runs past the section"; return false; }

	nodes.resize(count);
	for (uint16_t i = 0; i < count; i++)
	{
		const uint8_t *p = bytes + 4 + (size_t)i * 0x12u;
		SfxNode &n = nodes[i];
		n.x = (int16_t)Rd16(p);
		n.y = (int16_t)Rd16(p + 2);
		n.sound = Rd16(p + 4);
		n.key = Rd16(p + 6);
		n.flags = p[8];
		n.zone = p[9];
		n.range = Rd16(p + 0xa);
		n.period = Rd16(p + 0xc);
		n.timer = Rd16(p + 0xe);
		n.volume = p[0x10];
		n.unk11 = p[0x11];
	}

	// The zone list starts where the header says, not where the nodes end.
	// (They are the same place in all 44 retail maps.)
	if (zoneOffset < size)
	{
		const size_t zoneCount = (size - zoneOffset) / 0x20u;
		zones.resize(zoneCount);
		for (size_t z = 0; z < zoneCount; z++)
		{
			const uint8_t *p = bytes + zoneOffset + z * 0x20u;
			SfxZone &zone = zones[z];
			zone.count = (int)(int16_t)Rd16(p);
			for (int r = 0; r < 3; r++)
			{
				const uint8_t *q = p + 2 + r * 10;
				zone.rects[r].dampen = q[0];
				zone.rects[r].flags = q[1];
				zone.rects[r].xLo = (int16_t)Rd16(q + 2);
				zone.rects[r].yLo = (int16_t)Rd16(q + 4);
				zone.rects[r].xHi = (int16_t)Rd16(q + 6);
				zone.rects[r].yHi = (int16_t)Rd16(q + 8);
			}
		}
	}
	if (zoneOffset != 4u + (size_t)count * 0x12u)
		error = "zone offset does not follow the node list (parsed anyway)";
	return true;
}

void SfxSection::Setup()
{
	for (SfxNode &n : nodes)
		if (n.Active() && n.Mode() > 1)
			n.timer = (uint16_t)((uint16_t)(7u * n.period) >> 1);       // renderer.c:10145-10146
}

int SfxSection::FindByKey(uint16_t key) const
{
	for (size_t i = 0; i < nodes.size(); i++)
		if (nodes[i].key == key) return (int)i;
	return -1;
}

//==========================================================================
//
// The volume law
//
//==========================================================================

uint32_t SfxIsqrt(uint32_t v)
{
	uint32_t r = 0, bit = 1u << 30;
	while (bit > v) bit >>= 2;
	while (bit != 0)
	{
		if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return r & 0xffffu;                                  // the wrapper's movzx eax,ax (math_util.c:50)
}

int32_t SfxVolume(int32_t range, uint8_t vol8, const SfxZone *zone,
	int32_t emitterX, int32_t emitterY, int32_t playerX, int32_t playerY,
	uint32_t dist2, uint32_t master, uint32_t *distOut)
{
	const uint32_t dist = SfxIsqrt(dist2);               // audio.c:69
	if (distOut != nullptr) *distOut = dist;             // :70
	if (range == 0) return 0;                            // :72-80
	int32_t v = 0x7fff - (int32_t)(dist * 0x7fffu) / range;      // :81
	if (v < 0) return 0;                                 // :82
	if (vol8 != 0x40) v = (v * (int32_t)vol8) >> 6;      // :83-84

	if (zone != nullptr)                                 // :85-106
	{
		const int32_t relX = playerX - emitterX;
		const int32_t relY = playerY - emitterY;
		const int n = zone->count < 3 ? zone->count : 3;
		for (int i = 0; i < n; i++)
		{
			const SfxZoneRect &r = zone->rects[i];
			bool hit;
			if (r.flags & 1)                             // strictly inside
				hit = (r.xLo < relX) && (relX < r.xHi) && (r.yLo < relY) && (relY < r.yHi);
			else                                         // outside any bound
				hit = (relX < r.xLo) || (relX > r.xHi) || (relY < r.yLo) || (relY > r.yHi);
			if (hit)
			{
				v -= (int32_t)((uint32_t)r.dampen << 7);
				if (v < 0) return 0;
				break;                                   // the first hit ends the scan
			}
		}
	}

	if (master == 0) return 0;                           // :107
	// imul then an UNSIGNED shr (:108). A node volume above 0x40 amplifies, so
	// the product can pass 2^31: taken unsigned, as the 32-bit register holds it.
	v = (int32_t)(((uint32_t)v * master) >> 15);
	if (v > 0x7fff) v = 0x7fff;
	return v;
}

int32_t SfxClampDiff200(int32_t value, int32_t center)
{
	const int32_t diff = value - center;
	if (diff > 0x200) return center + 0x200;
	if (diff < -0x200) return center - 0x200;
	return value;
}

int32_t SfxLoopStartVolume(int32_t vol, uint16_t range)
{
	int32_t e = 0x7ff - (int32_t)range;                  // audio.c:878
	if (e < 0x10) e = 0x10;
	e <<= 4;
	// clamp_symmetric_26f2d(vol, 0, e), renderer.c:9919-9925.
	if (vol > e) return e;
	if (vol < -e) return -e;
	return vol;
}

//==========================================================================
//
// The node pump
//
//==========================================================================

std::vector<SfxQueryEntry> SfxNodePump(SfxSection &section, int32_t playerX, int32_t playerY,
	uint16_t frameScale, uint16_t rng16)
{
	std::vector<SfxQueryEntry> found;
	const int cap = 0x20;                                // audio.c:946
	for (size_t i = 0; i < section.nodes.size(); i++)
	{
		SfxNode &n = section.nodes[i];
		if (!n.Active()) continue;                       // :617
		const int mode = n.Mode();
		if (mode >= 2)                                   // :619-633
		{
			const uint16_t t = n.timer;
			n.timer = (uint16_t)(t - frameScale);
			if (t >= frameScale) continue;               // not expired yet
			uint16_t add;
			if (mode == 2)
				add = (uint16_t)((uint16_t)(n.period * 7) >> 1);
			else
				add = (uint16_t)((uint16_t)(7 * (((uint32_t)n.period * (uint32_t)rng16) >> 16)) >> 1);
			const uint32_t sum = (uint32_t)n.timer + add;
			n.timer = (uint16_t)sum;
			if (sum <= 0xffffu) n.timer = 0;             // no carry: clamp to 0
		}
		// 16-bit differences, as the original takes them (:635-636).
		const int16_t dx = (int16_t)(uint16_t)((uint16_t)playerX - (uint16_t)n.x);
		const int16_t dy = (int16_t)(uint16_t)((uint16_t)playerY - (uint16_t)n.y);
		const int32_t d2 = (int32_t)dy * dy + (int32_t)dx * dx;
		const uint32_t rng = n.range;
		if ((uint32_t)d2 > rng * rng) continue;          // :638, unsigned
		found.push_back({ (int)i, (uint32_t)d2 });
		if ((int)found.size() == cap) break;             // :642
	}

	// sort_sfx_query_by_distance: a bubble sort, ascending and SIGNED, which
	// keeps equal distances in node order. stable_sort gives the same order.
	std::stable_sort(found.begin(), found.end(), [](const SfxQueryEntry &a, const SfxQueryEntry &b)
	{ return (int32_t)a.dist2 < (int32_t)b.dist2; });
	return found;
}

//==========================================================================
//
// The songs
//
//==========================================================================

std::vector<uint8_t> ReadDbase300Chunk(const std::string &path, uint32_t index, std::string *why)
{
	std::vector<uint8_t> out;
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) { if (why) *why = "cannot open " + path; return out; }
	const long fileSize = FileSize(f);
	uint8_t sz[4];
	const uint64_t at = (uint64_t)index << 3;            // file_config.c:614
	if (fileSize < 0 || at + 4 > (uint64_t)fileSize || !ReadAt(f, at, sz, 4))
	{ fclose(f); if (why) *why = "chunk offset is past the end of the file"; return out; }
	const int32_t size = (int32_t)Rd32(sz);
	if (size > 0x11800 || size <= 0)                     // :617
	{ fclose(f); if (why) *why = "chunk size out of range"; return out; }
	if (at + 4 + (uint64_t)size > (uint64_t)fileSize)
	{ fclose(f); if (why) *why = "chunk runs past the end of the file"; return out; }
	out.resize((size_t)size);
	if (fread(out.data(), 1, out.size(), f) != out.size())
	{ out.clear(); if (why) *why = "short read"; }
	fclose(f);
	return out;
}

//==========================================================================
//
// The speech
//
//==========================================================================

void BuildDpcmStepTable(int32_t table[256])
{
	// renderer.c:10115-10132. Entry 0 is 0; then 127 pairs (+c, -c) with c
	// growing parabolically; then one last lone +c at entry 255.
	uint32_t eax = 0x40, edx = 0x2d, ecx = 0;
	table[0] = 0;
	int i = 1;
	while (i < 255)
	{
		ecx += eax >> 5;
		eax += edx;
		edx += 2;
		table[i] = (int32_t)ecx;
		table[i + 1] = -(int32_t)ecx;
		i += 2;
	}
	ecx += eax >> 5;
	table[255] = (int32_t)ecx;
}

bool ReadDbase500Header(const std::string &path, uint32_t clip, uint8_t hdr[0x2c])
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) return false;
	const bool ok = ReadAt(f, (uint64_t)clip << 3, hdr, 0x2c);
	fclose(f);
	return ok;
}

VoiceClip ReadDbase500Clip(const std::string &path, uint32_t clip, bool decode)
{
	VoiceClip v;
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) { v.error = "cannot open " + path; return v; }
	const long fileSize = FileSize(f);
	const uint64_t at = (uint64_t)clip << 3;             // audio.c:1238
	uint8_t hdr[0x2c];
	if (fileSize < 0 || at + sizeof(hdr) > (uint64_t)fileSize || !ReadAt(f, at, hdr, sizeof(hdr)))
	{ fclose(f); v.error = "clip header is past the end of the file"; return v; }

	v.tag = Rd16(hdr + 0x14);
	v.field18 = Rd32(hdr + 0x18);
	v.divisor = (int16_t)Rd16(hdr + 0x20);
	v.rawSize = Rd32(hdr + 0x28);
	v.dpcm = v.tag == 0x2a;                              // :1246
	if (v.field18 >= 4000 && v.field18 <= 48000) v.rate = (int)v.field18;

	if ((uint64_t)at + sizeof(hdr) + v.rawSize > (uint64_t)fileSize || v.rawSize == 0)
	{ fclose(f); v.error = "clip payload size out of range"; return v; }
	v.ok = true;
	if (!decode) { fclose(f); return v; }

	std::vector<uint8_t> raw(v.rawSize);
	if (fread(raw.data(), 1, raw.size(), f) != raw.size())
	{ fclose(f); v.ok = false; v.error = "short read"; return v; }
	fclose(f);

	if (v.dpcm)
	{
		int32_t table[256];
		BuildDpcmStepTable(table);
		uint32_t pred = 0;                               // :1248
		v.pcm.resize(raw.size());
		for (size_t i = 0; i < raw.size(); i++)
		{
			pred += (uint32_t)table[raw[i]];             // renderer.c:9951
			v.pcm[i] = (int16_t)pred;
		}
	}
	else
	{
		v.pcm.resize(raw.size() / 2);
		for (size_t i = 0; i < v.pcm.size(); i++) v.pcm[i] = (int16_t)Rd16(raw.data() + i * 2);
	}
	return v;
}

} // namespace roth
