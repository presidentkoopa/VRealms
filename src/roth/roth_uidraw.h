#pragma once
//
// The original's 2D drawing primitives, shared by every Realms panel that is
// drawn as the original draws it (roth_invdraw: the inventory panel;
// roth_menudraw: the message boxes and menus; roth_dlgdraw: the dialogue
// text). Engine-free: the C++ standard library, nothing else.
//
// A UiCanvas is a picture of one rectangle of the original's screen, in
// palette indices, plus a mask of the pixels the original's routines wrote
// there: a pixel nobody wrote is what lay behind on the original's screen
// (the 3D view, another panel) and is transparent in the picture.
//
// The routines address a VIRTUAL screen the way the original addresses its
// framebuffer (screen_xy_to_framebuffer_ptr, renderer.c:579-586): a linear
// address with a row pitch, so that what a blit or a glyph writes beside the
// rectangle lands beside it here too and is simply left out of the picture.
//
// WHERE IT COMES FROM (ROTH.C, roth_c/src/engine), routine by routine; the
// numbered checklist with file:line on both sides is in roth_uidraw.cpp:
//   screen_xy_to_framebuffer_ptr  renderer.c:579-586
//   resolve_reloc_ptr             renderer.c:1247-1254
//   blit_reloc_das_image          das_assets.c:1392-1400
//   blit_das_image_auto_scale     das_assets.c:1372-1376
//   blit_das_image_to_buffer      das_assets.c:1195-1367 (both variants, unshaded)
//   rle_decode_scroll_segment     blit_2d.c:296-327
//   clear_framebuffer_rect        renderer.c:1260-1274
//   fill_rect_solid               blit_2d.c:40-55
//   shade_remap_blit              renderer.c:11498-11523
//   draw_text_at_screen_xy        text_font.c:371-380
//   draw_text_to_buffer           text_font.c:300-367 (and its two helpers, :193-279)
//   measure_control_text_width    renderer.c:436-452
//

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace roth
{

// The original's font (g_font_descriptor, 0x70f12) is ROTH.EXE's own
// initialised data, not one of the game files. FindRothExeFont finds it in
// the bytes of the player's ROTH.EXE and accepts it only when it is exactly
// the original's (an FNV-1a hash of the bytes the font uses, measured from the
// running original by tools/invdraw_test); the bytes themselves are never
// copied into this program.
struct RothFont
{
	std::vector<uint8_t> bytes;     // the glyphs, then the descriptor
	size_t desc = 0;                // where the descriptor starts in `bytes`
	bool ok() const { return !bytes.empty(); }
};
// The span of the executable's data the font uses, around the descriptor: its
// glyphs lie BELOW it (the offset table is signed, renderer.c:342), the lowest
// at -0x4d6; the descriptor's 6 header bytes and 256 offsets end at +0x206.
static const size_t ROTH_FONT_BEFORE = 0x4d6;
static const size_t ROTH_FONT_AFTER = 0x206;
extern const uint32_t ROTH_FONT_HASH;          // measured from the running original; see roth_uidraw.cpp
uint32_t RothFontHash(const uint8_t *p, size_t n);
// Search `exe` for the font and verify it. False with `error` set when the
// file does not hold exactly the original's font.
bool FindRothExeFont(const std::vector<uint8_t> &exe, RothFont &out, std::string &error);

// rle_decode_scroll_segment's stream (blit_2d.c:296-327), whole: `total`
// bytes out of `src`; false when the stream ends first.
bool UiDecodeRle(const uint8_t *src, size_t avail, size_t total, std::vector<uint8_t> &out);
// Where a DAS picture's pixels start (das_assets.c:1197-1216; inventory.c:2079-2093).
size_t DasPayloadAt(const uint8_t *img);

// What the primitives read from outside.
struct UiAssets
{
	const std::vector<uint8_t> *iconsAll = nullptr;     // DATA\ICONS.ALL, whole (the reloc pictures, [0x7f56c])
	const uint8_t *shade = nullptr;     // the map pack's 0x4000 shade tables (Pack::ShadeTables): [0x86d14], the text ramp's FS ([0x90c0e]) and [0x86d28]
	const uint8_t *blend = nullptr;     // the map pack's 0x10000 table (Pack::BlendTable): [0x85d08]
	const RothFont *font = nullptr;     // null: no text is drawn (and Problems() says why)
};

class UiCanvas
{
public:
	// The picture covers screen x in [left, left + w), logical y in
	// [top, top + h); in the line-doubled (hires) modes every logical row is
	// two rows of the picture. Clears the picture and its mask.
	void SetRect(int left, int top, int w, int h, bool hires);
	void SetAssets(const UiAssets &a) { mA = a; }
	const UiAssets &Assets() const { return mA; }
	// g_screen_pitch (0x85498): only the packed-stride blit reads it (its row
	// advance is pitch - fill). 640 in the retail VideoMode 8.
	void SetScreenPitch(int p) { mScreenPitch = p; }

	int Left() const { return mLeft; }
	int Top() const { return mTop; }
	int Width() const { return mW; }
	int Height() const { return mHires ? mH * 2 : mH; }
	bool Hires() const { return mHires; }
	// Width() x Height() palette indices, row-major, and 1 where the
	// original's routines wrote the pixel.
	const std::vector<uint8_t> &Pixels() const { return mPix; }
	const std::vector<uint8_t> &Mask() const { return mMask; }
	// Bumped on every change to the picture.
	uint32_t Version() const { return mVersion; }
	// What went wrong, if anything (a missing picture, no font): one line each.
	const std::vector<std::string> &Problems() const { return mProblems; }

	// ---- the primitives, under the original's names --------------------------------
	// screen_xy_to_framebuffer_ptr (renderer.c:579-586): y doubled in hires.
	long Addr(int x, int y) const;
	void Put(long addr, uint8_t v);
	bool Get(long addr, uint8_t &v) const;      // false: outside the picture, or never written
	// resolve_reloc_ptr (renderer.c:1247-1254) on ICONS.ALL: the picture whose
	// table entry is at byte `slot`, and how many bytes of the file follow it.
	const uint8_t *RelocImage(uint32_t slot, size_t *avail);
	// blit_das_image_to_buffer (das_assets.c:1195-1367), unshaded (ecx = mode),
	// at `addr` with the row stride `ebx` as the original passes it: a plain
	// stride is the screen pitch; a packed one ((fill << 16) + pitch) selects
	// variant B. `rowsOverride` >= 0 stands for a height the caller patched into
	// the picture's header (menu_hud_ui.c:1474, dialogue_ui.c:1393).
	void BlitDasImageToBuffer(const uint8_t *img, size_t avail, long addr, uint32_t ebx, int mode, int rowsOverride = -1);
	// blit_reloc_das_image (das_assets.c:1392-1400) at screen (x, y) with
	// ebx = the screen pitch, or ((fill << 16) + pitch) for a packed blit.
	void BlitRelocDasImage(int x, int y, uint32_t slot, uint32_t fill = 0);
	// blit_das_image_auto_scale (das_assets.c:1372-1376) at screen (x, y).
	void BlitDasImageAutoScale(const uint8_t *img, size_t avail, int x, int y, int rowsOverride = -1);
	// clear_framebuffer_rect (renderer.c:1260-1274) and fill_rect_solid
	// (blit_2d.c:40-55): rows doubled in hires; at least one row.
	void ClearFramebufferRect(int x, int y, int w, int h) { FillRect(x, y, w, h, 0); }
	void FillRect(int x, int y, int w, int h, uint8_t color);
	// shade_remap_blit (renderer.c:11498-11523) over a block at screen (x, y),
	// `rows` picture rows (the caller doubles them in hires), table row `row`
	// of [0x86d28] (row != 0) -- the pack's shade table; see the checklist.
	void ShadeRemapBlit(int x, int y, int w, int rows, uint8_t row);
	// draw_text_at_screen_xy (text_font.c:371-380) and draw_text_to_buffer
	// (text_font.c:300-367). `len` bounds the read of `str`; the original
	// stops at its 0.
	void DrawTextAtScreenXy(const uint8_t *str, size_t len, int x, int y, uint32_t flags);
	void DrawTextToBuffer(const uint8_t *str, size_t len, long dest, uint32_t flags);
	// measure_control_text_width (renderer.c:436-452).
	int MeasureControlTextWidth(const uint8_t *s, size_t len) const;

	// A copy of a block of the picture and its mask (save_framebuffer_region,
	// blit_2d.c:1655) and putting it back (blit_descriptor_rows).
	struct Saved { int x = 0, y = 0, w = 0, rows = 0; std::vector<uint8_t> pix, mask; bool ok = false; };
	Saved SaveRegion(int x, int y, int w, int h) const;
	void RestoreRegion(const Saved &s);

	void Problem(const std::string &s);

	// The virtual screen: wide and tall enough that the screen and what is
	// written around it fit; never allocated.
	static const long VPITCH = 0x1000;
	static const long VORG = 0x200;

protected:
	UiAssets mA;
	int mLeft = 0, mTop = 0, mW = 0, mH = 0;
	bool mHires = false;
	int mScreenPitch = 640;
	std::vector<uint8_t> mPix, mMask;
	uint32_t mVersion = 0;
	std::vector<std::string> mProblems;
	// draw_text_to_buffer's colour ramp: a STATIC there (text_font.c:301), so a
	// string without its own 01 <colour> prefix takes the last one set. Here it
	// is one per canvas.
	uint8_t mRamp[32] = {};
};

} // namespace roth
