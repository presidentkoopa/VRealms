//
// The original's 2D drawing primitives. See roth_uidraw.h.
//
// THE CHECKLIST (porting rule 2). ROTH.C file:line -> here. These were the
// primitives of roth_invdraw.cpp (its checklist items 47-58), moved here
// unchanged so the menus and the dialogue text share them; the additions are
// marked NEW.
//
//  screen_xy_to_framebuffer_ptr, renderer.c:579-586
//   1  x + y * pitch, y doubled in the hi-res modes ......................... Addr
//  resolve_reloc_ptr, renderer.c:1247-1254 (and its byte-identical dup 0x18e2c)
//   2  NEW: base + dword[base + offset]; no base -> 0 ......................... RelocImage
//  blit_reloc_das_image, das_assets.c:1392-1400
//   3  image = base + dword[base + slot]; mode hires ? 0 : 1; ebx passed on .... BlitRelocDasImage
//  blit_das_image_auto_scale, das_assets.c:1372-1376
//   4  NEW: mode hires ? 0 : 1 ................................................. BlitDasImageAutoScale
//  blit_das_image_to_buffer, das_assets.c:1195-1367
//   5  header: +8 with flag 4, +0x300 without flag 2, then +8 ............... PayloadAt
//   6  row advance = ebx - width; > 0xfff -> variant B ......................... BlitDasImageToBuffer
//   7  NEW variant B: fill = advance >> 16, stride = (ebx & 0xffff) - fill; each row
//      ALWAYS through the run-length window (it never tests flag 1); the row's first
//      byte fills `fill` pixels; modes 2 (every other row), 0 (doubled), 1 ............. BlitDasImageToBuffer
//   8  run-length (flag 1): every pixel drawn, 0 included; modes 2 / 0 / 1 ........... BlitDasImageToBuffer
//   9  plain: modes 1 and 2 alike (1:1), 0 doubled ........................................ BlitDasImageToBuffer
//  10  the shaded variants (ecx bits 8-23) ......... not ported: no caller of this module passes a shade
//  11  NEW: the decode window reads on past the picture's own bytes (the stream is
//      one continuous run, das_assets.c:1222-1227): the rest of the file is passed ....... RelocImage avail
//  rle_decode_scroll_segment, blit_2d.c:296-327
//  12  < 0xf1 a literal, else (b - 0xf0) copies of the next byte; runs cross rows ...... DecodeRle
//  clear_framebuffer_rect, renderer.c:1260-1274 / fill_rect_solid, blit_2d.c:40-55
//  13  NEW: y offset and row count doubled in hires; bottom-tested: >= 1 row ........... FillRect
//  shade_remap_blit, renderer.c:11498-11523
//  14  NEW: row byte != 0 -> [0x86d28] + row << 8, else [0x86d14] + byte[0x11] << 8;
//      p = table[(base & ~0xff) | p]; bottom-tested loops ............................ ShadeRemapBlit
//      [0x86d28] == [0x86d14] in the running original (tools/menudraw_test, "worldoff=0"):
//      both are the pack's shade table here.
//  save_framebuffer_region, blit_2d.c:1655 / blit_descriptor_rows
//  15  NEW: a copy of the block and its putting back ...................................... SaveRegion / RestoreRegion
//  draw_text_at_screen_xy / draw_text_to_buffer, text_font.c:371-380, 300-367
//  16  hi-res: flags + 1, y doubled; pitch doubled; pen = dest + pitch + 1 .................. DrawTextAtScreenXy / DrawTextToBuffer
//  17  0 ends, 0x0d / 0x0a new line, 1 <c> ramp rows 0..11 of the shade table at column c
//      (flat with flag 8), 2 <w> advance ...................................................... DrawTextToBuffer
//  18  glyph: signed offset, 0 -> default advance; width, +1 with 0x20; top, rows;
//      nibble-packed when width <= 4 ........................................................... DrawTextToBuffer
//  19  outline: index 1 on 4 neighbours (6 in hi-res) unless flag 4 (text_font.c:193-245) .... DrawTextToBuffer
//  20  glyph pixels: ramp[top + row]; hi-res also the row below (text_font.c:252-279) ....... DrawTextToBuffer
//  21  the ramp is a static: a prefix-less string inherits the last colour ........... per canvas (mRamp)
//  blit_item_icon, inventory.c:2073-2148 (moved from roth_invdraw, its 45-46)
//  23  NEW here: header skips; ALWAYS run-length; index 0 not drawn; modes 2 / 1 / 0;
//      colour <= 0xff00 -> blend + colour, else shade + colour >> 8 ................ BlitItemIcon
//  load_item_icon_resource, inventory.c:973-996 (moved from roth_invdraw)
//  24  NEW here: the record's +0x0c resource of DBASE200: a size dword, the picture ...... ItemIconPicture
//  measure_control_text_width, renderer.c:436-452
//  22  sum of advances of bytes >= 0x20; 1 skips 1, 2 skips 2 ............................... MeasureControlTextWidth
//

#include "roth_uidraw.h"
#include "roth_dialogue.h"
#include "roth_dbase100.h"

#include <stdio.h>
#include <string.h>

namespace roth
{

// FNV-1a over [descriptor - ROTH_FONT_BEFORE, descriptor + ROTH_FONT_AFTER)
// of the running original's memory (tools/invdraw_test: the probe's font
// bytes, hashed by invdraw_diff "fonthash").
const uint32_t ROTH_FONT_HASH = 0x192939a9;

uint32_t RothFontHash(const uint8_t *p, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
	return h;
}

bool FindRothExeFont(const std::vector<uint8_t> &exe, RothFont &out, std::string &error)
{
	out = RothFont();
	// The descriptor's header: max char 0xff, line step 11, default advance 3
	// (text_font.c:304-306), then the offsets of the 14 control bytes, all 0.
	static const uint8_t head[6] = { 0xff, 0x00, 0x0b, 0x00, 0x03, 0x00 };
	const size_t span = ROTH_FONT_BEFORE + ROTH_FONT_AFTER;
	int candidates = 0;
	for (size_t i = ROTH_FONT_BEFORE; i + ROTH_FONT_AFTER <= exe.size(); i++)
	{
		if (memcmp(exe.data() + i, head, sizeof head) != 0) continue;
		candidates++;
		if (RothFontHash(exe.data() + i - ROTH_FONT_BEFORE, span) != ROTH_FONT_HASH) continue;
		out.bytes.assign(exe.begin() + (long)(i - ROTH_FONT_BEFORE), exe.begin() + (long)(i + ROTH_FONT_AFTER));
		out.desc = ROTH_FONT_BEFORE;
		return true;
	}
	char buf[160];
	snprintf(buf, sizeof buf, "ROTH.EXE does not hold the original's font (%d candidate header(s), none with the original's bytes)", candidates);
	error = buf;
	return false;
}

namespace
{

uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

} // namespace

// rle_decode_scroll_segment's stream (blit_2d.c:296-327), whole: the window
// the original refills is only a view of one continuous stream, a run
// crossing a row's end included.
bool UiDecodeRle(const uint8_t *src, size_t avail, size_t total, std::vector<uint8_t> &out)
{
	out.assign(total, 0);
	size_t o = 0, p = 0;
	while (o < total && p < avail)
	{
		const uint8_t b = src[p++];
		if (b < 0xf1) { out[o++] = b; continue; }
		if (p >= avail) break;
		const uint8_t v = src[p++];
		for (int r = b - 0xf0; r > 0 && o < total; r--) out[o++] = v;
	}
	return o == total;
}

// The header of a DAS picture (das_assets.c:1197-1216): where its pixels start.
size_t DasPayloadAt(const uint8_t *img)
{
	size_t p = 0;
	if (img[0] & 4) p += 8;
	if (!(img[0] & 2)) p += 0x300;
	return p + 8;
}

//==========================================================================
//
// The picture.
//
//==========================================================================

void UiCanvas::SetRect(int left, int top, int w, int h, bool hires)
{
	mLeft = left;
	mTop = top;
	mW = w < 0 ? 0 : w;
	mH = h < 0 ? 0 : h;
	mHires = hires;
	mPix.assign((size_t)mW * Height(), 0);
	mMask.assign((size_t)mW * Height(), 0);
	mRuns.clear();
	mVersion++;
}

static bool gTextAsRuns = false;
void UiSetTextAsRuns(bool on) { gTextAsRuns = on; }
bool UiTextAsRuns() { return gTextAsRuns; }

void UiCanvas::DropRuns(int x, int y, int w, int h)
{
	for (size_t i = 0; i < mRuns.size();)
	{
		const UiTextRun &r = mRuns[i];
		const bool hit = r.x < x + w && x < r.x + r.w && r.y < y + h && y < r.y + r.h;
		if (hit) { mRuns.erase(mRuns.begin() + (long)i); mVersion++; }
		else i++;
	}
}

void UiCanvas::AddRun(const UiTextRun &r)
{
	if (r.text.empty() || r.w <= 0 || r.h <= 0) return;
	DropRuns(r.x, r.y, r.w, r.h);
	mRuns.push_back(r);
	mVersion++;
}

void UiCanvas::Problem(const std::string &s)
{
	for (const std::string &p : mProblems) if (p == s) return;
	mProblems.push_back(s);
}

long UiCanvas::Addr(int x, int y) const
{
	const long yy = mHires ? (long)y * 2 : (long)y;
	return (VORG + yy) * VPITCH + VORG + x;
}

void UiCanvas::Put(long addr, uint8_t v)
{
	const long y = addr / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop);
	const long x = addr % VPITCH - VORG - mLeft;
	if (x < 0 || x >= mW || y < 0 || y >= Height()) return;
	const size_t i = (size_t)(y * mW + x);
	if (mPix[i] != v || mMask[i] != 1) { mPix[i] = v; mMask[i] = 1; mVersion++; }
}

bool UiCanvas::Get(long addr, uint8_t &v) const
{
	const long y = addr / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop);
	const long x = addr % VPITCH - VORG - mLeft;
	if (x < 0 || x >= mW || y < 0 || y >= Height()) return false;
	const size_t i = (size_t)(y * mW + x);
	if (mMask[i] != 1) return false;
	v = mPix[i];
	return true;
}

UiCanvas::Saved UiCanvas::SaveRegion(int x, int y, int w, int h) const
{
	Saved s;
	s.x = x; s.y = y; s.w = w; s.rows = mHires ? h * 2 : h;
	s.pix.assign((size_t)w * s.rows, 0);
	s.mask.assign((size_t)w * s.rows, 0);
	const long a0 = Addr(x, y);
	for (int r = 0; r < s.rows; r++)
		for (int c = 0; c < w; c++)
		{
			const long a = a0 + (long)r * VPITCH + c;
			const long yy = a / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop);
			const long xx = a % VPITCH - VORG - mLeft;
			if (xx < 0 || xx >= mW || yy < 0 || yy >= Height()) continue;
			const size_t i = (size_t)(yy * mW + xx);
			s.pix[(size_t)r * w + c] = mPix[i];
			s.mask[(size_t)r * w + c] = mMask[i];
		}
	s.ok = true;
	return s;
}

void UiCanvas::RestoreRegion(const Saved &s)
{
	if (!s.ok) return;
	const long a0 = Addr(s.x, s.y);
	if (!mRuns.empty())
		DropRuns((int)(a0 % VPITCH - VORG - mLeft), (int)(a0 / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop)), s.w, s.rows);
	for (int r = 0; r < s.rows; r++)
		for (int c = 0; c < s.w; c++)
		{
			const long a = a0 + (long)r * VPITCH + c;
			const long yy = a / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop);
			const long xx = a % VPITCH - VORG - mLeft;
			if (xx < 0 || xx >= mW || yy < 0 || yy >= Height()) continue;
			const size_t i = (size_t)(yy * mW + xx);
			const uint8_t p = s.pix[(size_t)r * s.w + c], m = s.mask[(size_t)r * s.w + c];
			if (mPix[i] != p || mMask[i] != m) { mPix[i] = p; mMask[i] = m; mVersion++; }
		}
}

//==========================================================================
//
// Pictures.
//
//==========================================================================

const uint8_t *UiCanvas::RelocImage(uint32_t slot, size_t *avail)
{
	if (mA.iconsAll == nullptr || mA.iconsAll->empty()) { Problem("no ICONS.ALL"); return nullptr; }   // base 0: nothing
	const std::vector<uint8_t> &f = *mA.iconsAll;
	if ((size_t)slot + 4 > f.size()) { Problem("an ICONS.ALL slot past the table"); return nullptr; }
	const uint32_t off = U32(f.data() + slot);
	if ((size_t)off + 8 > f.size()) { Problem("an ICONS.ALL picture past the file"); return nullptr; }
	*avail = f.size() - off;
	return f.data() + off;
}

void UiCanvas::BlitDasImageToBuffer(const uint8_t *img, size_t avail, long addr, uint32_t ebx, int mode, int rowsOverride)
{
	if (img == nullptr || avail < 8) return;
	const uint32_t width = U16(img + 4);
	int rows = rowsOverride >= 0 ? rowsOverride : (int)U16(img + 6);
	const size_t p = DasPayloadAt(img);
	if (width == 0 || rows <= 0 || p > avail) return;
	const uint32_t advance = ebx - width;                                   // [f+0x1c], :1213
	std::vector<uint8_t> px;
	long a = addr;
	if (advance > 0xfff)                                                    // :1229-1265, variant B
	{
		const uint32_t fill = advance >> 16;
		const long stride = (long)(ebx & 0xffff) - (long)fill;              // :1232; the address steps by fill + stride
		const long pitchHere = VPITCH;                                      // one row of the virtual screen
		(void)stride;
		if (!UiDecodeRle(img + p, avail - p, (size_t)width * rows, px)) Problem("a picture's run-length stream ends early");
		auto row = [&](int r) { for (uint32_t n = 0; n < fill; n++) Put(a + (long)n, px[(size_t)r * width]); };
		if (mode == 2)
		{
			bool skip = false;
			for (int r = 0; r < rows; r++) { if (!skip) { row(r); a += pitchHere; } skip = !skip; }
		}
		else if (mode == 0)
		{
			for (int r = 0; r < rows; r++) { row(r); a += pitchHere; row(r); a += pitchHere; }
		}
		else
		{
			for (int r = 0; r < rows; r++) { row(r); a += pitchHere; }
		}
		return;
	}
	if (img[0] & 1)
	{
		if (!UiDecodeRle(img + p, avail - p, (size_t)width * rows, px)) Problem("a picture's run-length stream ends early");
	}
	else
	{
		px.assign((size_t)width * rows, 0);
		for (size_t i = 0; i < px.size() && p + i < avail; i++) px[i] = img[p + i];
		mode = mode == 0 ? 0 : 1;                                           // :1348-1356: modes 1 and 2 alike
	}
	if (mode == 2)                                                          // every other row
	{
		for (int r = 0; r < rows; r += 2, a += VPITCH)
			for (uint32_t c = 0; c < width; c++) Put(a + c, px[(size_t)r * width + c]);
	}
	else if (mode == 0)                                                     // doubled rows
	{
		for (int r = 0; r < rows; r++, a += 2 * VPITCH)
			for (uint32_t c = 0; c < width; c++) { Put(a + c, px[(size_t)r * width + c]); Put(a + VPITCH + c, px[(size_t)r * width + c]); }
	}
	else
	{
		for (int r = 0; r < rows; r++, a += VPITCH)
			for (uint32_t c = 0; c < width; c++) Put(a + c, px[(size_t)r * width + c]);
	}
}

void UiCanvas::BlitRelocDasImage(int x, int y, uint32_t slot, uint32_t fill)
{
	size_t avail = 0;
	const uint8_t *img = RelocImage(slot, &avail);
	if (img == nullptr) return;
	const uint32_t ebx = (fill << 16) + (uint32_t)mScreenPitch;
	BlitDasImageToBuffer(img, avail, Addr(x, y), ebx, mHires ? 0 : 1);
}

void UiCanvas::BlitDasImageAutoScale(const uint8_t *img, size_t avail, int x, int y, int rowsOverride)
{
	BlitDasImageToBuffer(img, avail, Addr(x, y), (uint32_t)mScreenPitch, mHires ? 0 : 1, rowsOverride);
}

void UiCanvas::FillRect(int x, int y, int w, int h, uint8_t color)
{
	int rows = h;
	if (mHires) rows += rows;
	long a = Addr(x, y);
	if (!mRuns.empty())
		DropRuns((int)(a % VPITCH - VORG - mLeft), (int)(a / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop)), w, rows < 1 ? 1 : rows);
	do {
		for (int c = 0; c < w; c++) Put(a + c, color);
		a += VPITCH;
	} while (--rows > 0);
}

void UiCanvas::ShadeRemapBlit(int x, int y, int w, int rows, uint8_t row)
{
	if (mA.shade == nullptr) { Problem("no shade table: the fade is not drawn"); return; }
	const size_t base = (size_t)row << 8;
	long a = Addr(x, y);
	int h = rows;
	do {
		int c = w;
		long e = a;
		do {
			const long yy = e / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop);
			const long xx = e % VPITCH - VORG - mLeft;
			if (xx >= 0 && xx < mW && yy >= 0 && yy < Height())
			{
				const size_t i = (size_t)(yy * mW + xx);
				if (mMask[i] == 1)
				{
					const size_t t = base | mPix[i];
					const uint8_t v = t < 0x4000 ? mA.shade[t] : mPix[i];
					if (mPix[i] != v) { mPix[i] = v; mVersion++; }
				}
				else if (mMask[i] == 0) { mMask[i] = (uint8_t)(0x20 + (row & 0x1f)); mVersion++; }   // what lay behind, shaded (see Mask())
				else Problem("a block shaded twice over what lay behind it");
			}
			e++;
		} while (--c > 0);
		a += VPITCH;
	} while (--h > 0);
}

// blit_item_icon (inventory.c:2073-2148) at screen (x, y). Moved here from
// roth_invdraw.cpp (its checklist items 45-46) for the HUD's corner icon. `ecx` is the
// original's mode word: low byte the row mode, bits 8-23 the tint.
void UiCanvas::BlitItemIcon(const uint8_t *icon, size_t size, int x, int y, uint32_t ecx)
{
	if (icon == nullptr || size < 8) return;
	const int w = U16(icon + 4), rows = U16(icon + 6);
	const uint32_t color = ecx & 0xffff00u;
	const int mode = (int)(ecx & 0xff);
	const size_t p = DasPayloadAt(icon);
	if (w <= 0 || rows <= 0 || p > size) return;
	std::vector<uint8_t> px;
	UiDecodeRle(icon + p, size - p, (size_t)w * rows, px);                   // :2093-2147: always the run-length decoder
	const uint8_t *table = nullptr;
	uint32_t base = 0;
	size_t tableSize = 0;
	if (color != 0)                                                         // :2121-2123
	{
		if (color <= 0xff00u) { table = mA.blend; base = color; tableSize = 0x10000; }
		else { table = mA.shade; base = color >> 8; tableSize = 0x4000; }
		if (table == nullptr) { Problem("no shade or blend table in the map's pack: tinted icons are drawn untinted"); }
	}
	auto ink = [&](uint8_t v) -> uint8_t {
		if (table == nullptr) return v;
		const size_t i = (size_t)(base & ~0xffu) + v;                      // cmap_lut, inventory.c:2038-2041
		return i < tableSize ? table[i] : v;
	};
	long a = Addr(x, y);
	const long stride = VPITCH - w;
	if (mode == 2)                                                          // :2096-2103, 2124-2131
	{
		for (int r = 0; r < rows; r += 2)
		{
			for (int c = 0; c < w; c++, a++) { const uint8_t v = px[(size_t)r * w + c]; if (v) Put(a, ink(v)); }
			a += stride;
		}
	}
	else if (mode == 0)                                                     // :2104-2111, 2132-2139
	{
		const long second = color == 0 ? w : stride + w;
		for (int r = 0; r < rows; r++)
		{
			for (int c = 0; c < w; c++, a++) { const uint8_t v = px[(size_t)r * w + c]; if (v) { Put(a, ink(v)); Put(a + second, ink(v)); } }
			a += color == 0 ? w + stride : stride;
		}
	}
	else
	{
		for (int r = 0; r < rows; r++)
		{
			for (int c = 0; c < w; c++, a++) { const uint8_t v = px[(size_t)r * w + c]; if (v) Put(a, ink(v)); }
			a += stride;
		}
	}
}

// load_item_icon_resource (inventory.c:973-996): an item's icon, its
// record's +0x0c resource of DBASE200.DAT, a dword size and the picture.
// Moved here from roth_invdraw.cpp (InventoryPanelPainter::ItemIcon).
bool ItemIconPicture(const Dbase100 &db, const std::vector<uint8_t> &d, int32_t itemId, const uint8_t **img, size_t *size)
{
	if (itemId <= 0) return false;
	const DbItem *rec = db.Item(itemId);
	if (rec == nullptr || !rec->ok || rec->icon == 0) return false;
	const uint64_t off = (uint64_t)rec->icon << 3;
	if (off + 4 > d.size()) return false;
	const uint32_t n = U32(d.data() + off);
	if (n < 8 || off + 4 + n > d.size()) return false;
	*img = d.data() + off + 4;
	*size = n;
	return true;
}

//==========================================================================
//
// Text.
//
//==========================================================================

int UiCanvas::MeasureControlTextWidth(const uint8_t *s, size_t len) const
{
	int width = 0;
	for (size_t i = 0; i < len; i++)
	{
		const uint8_t c = s[i];
		if (c == 0) break;
		if (c == 1) i += 1;
		else if (c == 2) i += 2;
		else if (c >= 0x20) width += roth::FontCharAdvance(c);
	}
	return width;
}

void UiCanvas::DrawTextAtScreenXy(const uint8_t *str, size_t len, int x, int y, uint32_t flags)
{
	if (mHires) flags += 1;                                                 // :373-376 (y is doubled by Addr)
	DrawTextToBuffer(str, len, Addr(x, y), flags);
}

void UiCanvas::DrawTextToBuffer(const uint8_t *str, size_t len, long dest, uint32_t flags)
{
	if (mA.font == nullptr || !mA.font->ok()) { Problem("no font: text is not drawn (ROTH.EXE's font was not found)"); return; }
	const std::vector<uint8_t> &fb = mA.font->bytes;
	const long D = (long)mA.font->desc;
	auto fbyte = [&](long i) -> uint8_t { return (i >= 0 && i < (long)fb.size()) ? fb[(size_t)i] : 0; };
	auto fword = [&](long i) -> uint16_t { return (uint16_t)(fbyte(i) | (fbyte(i + 1) << 8)); };
	if (gTextAsRuns) { TextRuns(str, len, dest, flags); return; }
	long pitch = VPITCH;
	if (flags & 1) pitch += pitch;                                          // :303
	const uint32_t maxChar = fword(D), lineStep = fword(D + 2), defAdv = fword(D + 4);
	const long newlineAdv = (long)lineStep * pitch;
	long pen = dest + pitch + 1, lineBase = pen;                            // :308-309
	const int dbl = (flags & 1) != 0;
	const long half = pitch >> 1;
	const long esi = dbl ? (pitch >> 1) : pitch;                            // outline step, :199
	auto mark = [&](long e) {
		if (dbl) { Put(e + esi - 1, 1); Put(e + esi + 1, 1); Put(e - 1, 1); Put(e + 1, 1); Put(e - esi, 1); Put(e + 2 * esi, 1); }
		else { Put(e - 1, 1); Put(e + 1, 1); Put(e - esi, 1); Put(e + esi, 1); }
	};
	for (size_t i = 0; i < len; i++)
	{
		const uint8_t c = str[i];
		if (c == 0) return;
		if (c == 0x0d) { pen = lineBase + newlineAdv; lineBase = pen; continue; }
		if (c < 0x0d)
		{
			if (c == 1)
			{
				const uint8_t color = i + 1 < len ? str[++i] : 0;
				for (int k = 0; k < 12; k++)
					mRamp[k] = (flags & 8) ? color : (mA.shade ? mA.shade[(k << 8) | color] : color);   // fs:[(k<<8)|colour]
			}
			else if (c == 2)
			{
				const uint16_t adv = (uint16_t)((i + 1 < len ? str[i + 1] : 0) | ((i + 2 < len ? str[i + 2] : 0) << 8));
				i += 2;
				pen += adv;
			}
			else if (c == 0x0a) { pen = lineBase + newlineAdv; lineBase = pen; }
			continue;
		}
		if (c > maxChar) { pen += defAdv; continue; }
		const int16_t goff = (int16_t)fword(D + 6 + c * 2);
		if (goff == 0) { pen += defAdv; continue; }
		const long g = D + goff;
		const uint16_t metrics = fword(g);
		long rows = g + 2;
		const uint8_t mlow = (uint8_t)(metrics & 0xff), mhigh = (uint8_t)(metrics >> 8);
		const long adv = (mlow & 0x0f) + 1;
		int8_t width = (int8_t)(mlow & 0x0f);
		if (mlow & 0x20) width++;
		const int top = mhigh >> 4;
		const int rowCount = (mhigh & 0x0f) + 1;
		const long blitPen = pen + (long)top * pitch;
		for (int pass = (flags & 4) ? 1 : 0; pass < 2; pass++)              // 0: the outline, 1: the glyph
		{
			long rp = rows, e0 = blitPen;
			int ri = top;
			uint8_t dl = 0, cl = 0;
			for (int rc = rowCount; rc > 0; rc--)
			{
				const uint8_t ch = mRamp[ri & 31];
				ri++;
				if (width <= 4)
				{
					if (dl == 0) cl = fbyte(rp++);
					dl = (uint8_t)~dl;
					if (cl & 0xf0)
					{
						long e = e0;
						int8_t dh = width;
						do {
							const int carry = cl & 0x80; cl = (uint8_t)(cl << 1);
							if (carry) { if (pass == 0) mark(e); else { Put(e, ch); if (dbl) Put(e + half, ch); } }
							e++;
						} while (--dh > 0);
					}
					else cl = (uint8_t)(cl << 4);
				}
				else
				{
					cl = fbyte(rp++);
					if (cl != 0)
					{
						long e = e0;
						int8_t dh = width;
						do {
							const int carry = cl & 0x80; cl = (uint8_t)(cl << 1);
							if (carry) { if (pass == 0) mark(e); else { Put(e, ch); if (dbl) Put(e + half, ch); } }
							e++;
						} while (--dh > 0);
					}
				}
				e0 += pitch;
			}
		}
		pen += adv;
	}
}

// draw_text_to_buffer's walk, the same pen arithmetic as above (text_font.c:300-367), writing
// no pixel: each stretch of one line in one colour becomes a run (UiSetTextAsRuns).
void UiCanvas::TextRuns(const uint8_t *str, size_t len, long dest, uint32_t flags)
{
	const std::vector<uint8_t> &fb = mA.font->bytes;
	const long D = (long)mA.font->desc;
	auto fbyte = [&](long i) -> uint8_t { return (i >= 0 && i < (long)fb.size()) ? fb[(size_t)i] : 0; };
	auto fword = [&](long i) -> uint16_t { return (uint16_t)(fbyte(i) | (fbyte(i + 1) << 8)); };
	long pitch = VPITCH;
	if (flags & 1) pitch += pitch;
	const uint32_t maxChar = fword(D), lineStep = fword(D + 2), defAdv = fword(D + 4);
	const long newlineAdv = (long)lineStep * pitch;
	long pen = dest + pitch + 1, lineBase = pen;
	const int rowMul = (flags & 1) ? 2 : 1;
	auto px = [&](long a) { return (int)(a % VPITCH - VORG - mLeft); };
	auto py = [&](long a) { return (int)(a / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop)); };
	UiTextRun run;
	bool open = false;
	int x1 = 0;
	auto flush = [&]() {
		if (open)
		{
			while (!run.text.empty() && run.text.back() == ' ') run.text.pop_back();
			run.w = x1 - run.x;
			AddRun(run);
		}
		open = false;
		run = UiTextRun();
	};
	auto put = [&](char ch, int top, int rowCount, long adv) {
		if (!open)
		{
			if (ch == ' ') return;          // a run starts at its first glyph
			open = true;
			run.x = px(pen);
			run.y = py(lineBase) - rowMul;  // the pen starts one row below the line's top (pen = dest + pitch + 1)
			run.h = (int)lineStep * rowMul;
			run.colour = mRamp[(top + rowCount / 2) & 31];
		}
		run.text.push_back(ch);
		x1 = px(pen) + (int)adv;
	};
	for (size_t i = 0; i < len; i++)
	{
		const uint8_t c = str[i];
		if (c == 0) break;
		if (c == 0x0d || c == 0x0a) { flush(); pen = lineBase + newlineAdv; lineBase = pen; continue; }
		if (c < 0x0d)
		{
			if (c == 1)
			{
				flush();
				const uint8_t color = i + 1 < len ? str[++i] : 0;
				for (int k = 0; k < 12; k++)
					mRamp[k] = (flags & 8) ? color : (mA.shade ? mA.shade[(k << 8) | color] : color);
			}
			else if (c == 2)
			{
				const uint16_t adv = (uint16_t)((i + 1 < len ? str[i + 1] : 0) | ((i + 2 < len ? str[i + 2] : 0) << 8));
				i += 2;
				flush();
				pen += adv;
			}
			continue;
		}
		if (c > maxChar) { if (c == ' ') put(' ', 0, 1, (long)defAdv); pen += defAdv; continue; }
		const int16_t goff = (int16_t)fword(D + 6 + c * 2);
		if (goff == 0) { put(' ', 0, 1, (long)defAdv); pen += defAdv; continue; }
		const uint16_t metrics = fword(D + goff);
		const uint8_t mlow = (uint8_t)(metrics & 0xff), mhigh = (uint8_t)(metrics >> 8);
		const long adv = (mlow & 0x0f) + 1;
		put((char)c, mhigh >> 4, (mhigh & 0x0f) + 1, adv);
		pen += adv;
	}
	flush();
}

} // namespace roth
