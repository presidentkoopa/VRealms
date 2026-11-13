//
// Realms of the Haunting's HUD, drawn as the original draws it. See
// roth_huddraw.h.
//
// THE CHECKLIST (porting rule 2). ROTH.C file:line -> here. "caller" = done by
// whoever drives the painter (the engine, roth_interface.cpp TickHud; the test,
// tools/huddraw_test/huddraw_diff.cpp), at the same point as the original.
// Abbreviations: MH menu_hud_ui.c, WC weapon_combat.c, B2 blit_2d.c,
// INV inventory.c, GC game_core.c, R renderer.c.
//
//  refresh_hud_layout, MH:426-459
//   1  :428 free the old health panel ............................... RefreshHudLayout
//   2  :431-433 full view: (1, 8) -> the panel at column 8, row 1 (blit_panel_image reads +0x44 = 8) ... RefreshHudLayout
//   3  :434-448 the windowed layout (per-resolution table 0x71390) ... not ported: full view only (header)
//   4  :453-458 a 0x4b0-byte buffer, then render_health_status_panel ... RefreshHudLayout
//  render_health_status_panel, MH:299-349
//   5  :304-309 the panel record (pitch, rows, screen place) ........ RenderHealthStatusPanel (mHealthX)
//   6  :311 the buffer cleared, pitch * rows bytes .................. RenderHealthStatusPanel
//   7  :313-314 the panel picture d[4] at (d[5], d[6]) ............... RenderHealthStatusPanel
//   8  :316-318 the bar picture d[7]; d[8] resolved, unused ........... RenderHealthStatusPanel
//   9  :320-328 health clamped to [0, max] (unsigned); fill = d[11] * health / max; 1 if 0 with health ... RenderHealthStatusPanel
//  10  :330-331 fill below half: ICONS.ALL 0x298 instead ............... RenderHealthStatusPanel
//  11  :333-343 the picture into the work buffer (pitch 0x64), fill bytes of d[14] rows to (d[9], d[12]) ... WorkRows
//  12  :345-348 dirty rect ........................................... not ported: VGA page bookkeeping
//  render_player_health_bar, MH:357-418
//  13  :362 the redraw flag (0x83e7c) cleared .......................... RenderPlayerHealthBar
//  14  :363-372 clamp and fill as 9 (no max == 0 guard in the original: a Problem here) ... RenderPlayerHealthBar
//  15  :376-389 the filled part, 0x298 below half else d[7] ........... RenderPlayerHealthBar
//  16  :391-405 the empty part: d[8] from column `fill` of the work buffer ... RenderPlayerHealthBar
//  17  :407-417 dirty rect ........................................... not ported: VGA bookkeeping
//  render_weapon_hud, WC:994-1058
//  18  :998 the panel's descriptor pointer [0x83e78] = 0 .............. RenderWeaponHud
//  19  :999-1000 0x83d70 = (pitch == 640) .............................. not needed: read by the windowed paths only
//  20  :1001-1013 the windowed save-under freed, dirty rects .......... not ported: the full view never saves one (MH:757-763)
//  21  :1015-1016 panel number > 0xf -> 0, written back into the weapon's table ... RenderWeaponHud (returns it; the caller writes it)
//  22  :1017-1024 panel 0: both panel buffers freed, +0x1c = 0, descriptor 0, return (no rebuild) ... RenderWeaponHud
//  23  :1025-1026 table index = panel - 1; the old buffers freed ....... RenderWeaponHud
//  24  :1028-1043 full view: y 1, x (pitch - 0x90) >> 1; windowed: save-under ... RenderWeaponHud (full view only)
//  25  :1048-1055 a 0x1290-byte buffer, descriptor = 0x718b8[panel - 1], render_ui_texture_panel ... RenderWeaponHud
//  26  :1057 rebuild_weapon_inventory_list ........................... caller (logic: ActorWorld::RebuildWeaponList)
//  render_ui_texture_panel, MH:523-622
//  27  :528 the panel's redraw flag (0x83e80) cleared .................. RenderUiTexturePanel
//  28  :529-543 copy width d[0], rows d[1] + 3; full view: column (pitch >> 1) - d[2] ... RenderUiTexturePanel (mWeaponX)
//  29  :544 the buffer cleared, (d[1] + 3) * d[0] bytes ............... RenderUiTexturePanel
//  30  :546-547 the panel picture d[4] at (d[5], d[6]) ................. RenderUiTexturePanel
//  31  :549-550 d[7], and d[8] when set ................................ RenderUiTexturePanel
//  32  :556-572 mode 0: the cells (item >> 8 when the cap > 2; the count into 0x83d7c), d[10] across x d[13] rows, step d[11] / d[14]; past the count d[8] or stop the row ... RenderUiTexturePanel
//  33  :573-586 mode 1: item clamped to 0x100, width d[11] * item >> 8 of d[7] from the left ... RenderUiTexturePanel
//  34  :587-602 mode 2: the same width placed at the right end ........... RenderUiTexturePanel
//  35  :605-616 with clips carried: picture d[15] at (d[16], d[17]); with two or more and d[18] set, "\x9e%D" in colour 0x7675e at (d[18], d[19]) ... RenderUiTexturePanel
//  36  :618-621 dirty rect ........................................... not ported: VGA bookkeeping
//  render_weapon_view, WC:1224-1369
//  37  :1228-1229 no descriptor: nothing ............................... RenderWeaponView
//  38  :1237-1248 mode 1: d[7] for `scaled`, then d[8] for the rest from column `scaled` ... RenderWeaponView
//  39  :1249-1260 mode 2: d[8] for the rest, then d[7] at +scaled from column `scaled` (as the lift reads the original) ... RenderWeaponView
//  40  :1262-1310 mode 0, arg <= 0: the counter down by one; the cell it names (wrapping once to the second row) blanked with d[8], or cleared with d[7]'s size ... RenderWeaponView
//  41  :1311-1341 mode 0, arg 1: every cell redrawn from the item (>> 8 when the cap > 2): d[7], past the count d[8] or cleared ... RenderWeaponView
//  42  :1343-1348 mode 3: nothing ...................................... RenderWeaponView
//  43  :1352-1365 dirty rect ........................................... not ported: VGA bookkeeping
//  redraw_weapon_hud_panel, WC:540-553
//  44  with a panel buffer: render_ui_texture_panel ..................... RedrawWeaponHudPanel
//  draw_active_ui_panels, MH:749-775
//  45  :751 tick_weapon_hud_ammo_anim .................................. DrawFrame calls the caller's `tick` there (ActorWorld::RechargeFrame; its redraws reach RenderWeaponView(1))
//  46  :752-754 weapon panel: render_ui_texture_panel while 0x83e80 is set ... DrawActiveUiPanels
//  47  :755-766 render_ui_panel_text, full view (MH:664-686): the buffer's non-zero bytes at (column, row 1), width x rows ... DrawActiveUiPanels
//  48  :768-771 health panel: render_player_health_bar while 0x83e7c is set ... DrawActiveUiPanels
//  49  :772-773 blit_panel_image, full view (B2:1405-1429): non-zero bytes at (column 8, row 1), pitch x rows ... DrawActiveUiPanels
//  50  copy_nonzero_bytes (R:9651-9660): from count - 1 down to 0, nothing when count <= 1 ... DrawActiveUiPanels (copyNonzero)
//  51  the hi-res (line-doubled) copies .................................. not ported: VideoMode 8 does not double lines
//  gameplay_frame_step's drawing, GC:494-500
//  52  draw_held_item_icon, draw_active_ui_panels, then the corner while 0x7fdc0 is set ... DrawFrame
//  53  render_dev_map_selector_ui, render_text_ui(0) ..................... not here: the developers' map picker; the text is the dialogue's (roth_dialogue)
//  54  :501 restore_corner_peek_icon after the flip ...................... not ported: it puts the saved view back after the frame is shown; every shown frame is drawn from the view up
//  run_gameplay_frame's corner timer, GC:520-533; clear_corner_peek_icon MH:232-241
//  55  pointer in the top left 0x20 x 0x20: timer + fts, at most 0x19; else down by fts, at 0 or below cleared to 0 ... CornerPeekTimer
//  draw_character_portrait_corner, MH:262-288
//  56  :264-271 the save-under taken (or re-taken) ........................ not ported: no visible effect (see 54)
//  57  :272-276 timer < 10: a growing box, at 0x12 - (t + 4), size (t + 4) * 2: shadow, then cleared ... DrawCharacterPortraitCorner
//  58  :277-286 else the box at (4, 4) 0x1c square, shadow, cleared, and Adam's item's icon fading in: tint (0xf - (t - 10)) << 9 while t - 10 < 0xe ... DrawCharacterPortraitCorner
//  draw_popup_shadow_border_smc, B2:1983-2008
//  59  top edge when y > 0, bottom when y + h < the screen's height, left when x > 0, right when x + w < pitch; four corner blocks always ... DrawPopupShadowBorder
//  60  hi-res self-modifying immediates ................................. not ported: VideoMode 8 does not double lines
//  draw_shadow_border_edge_h, B2:159-195
//  61  rows = min(pitch - x, 4) gradient bytes; a row with a negative offset skipped; `count` pixels a row, at least one ... ShadowEdgeH
//  draw_shadow_border_edge_v, B2:205-224
//  62  four columns; a column with a negative offset skipped; `height` rows, at least one ... ShadowEdgeV
//  blit_translucent_overlay_block, B2:108-150
//  63  4 x 4, a row when (unsigned) y < height, a pixel when (unsigned) x < pitch ... OverlayBlock
//  64  the blend: fs:[(gradient << 8) | pixel], fs = the shade tables (selector 0x90c0e = 0x86d14) ... UiCanvas::ShadeRemapBlit (one row / column / pixel at a time)
//  clear_framebuffer_rect, R:1260-1271
//  65  width zeroes a row, at least one row .............................. UiCanvas::ClearFramebufferRect
//  draw_item_icon_centered, INV:1202-1211
//  66  x + (0x1c - w) >> 1, y + (0x38 - h) >> 2, mode (tint << 8) | 2 (1 in hi-res) ... DrawCharacterPortraitCorner (UiCanvas::BlitItemIcon, moved there from roth_invdraw)
//  update_selected_item_icon, INV:1461-1493
//  67  :1464 the slide timer reset .......................................... UpdateSelectedItemIcon
//  68  :1465-1468 no selection or not enabled (0x819b8): rows 0, id 0 ......... UpdateSelectedItemIcon
//  69  :1471 the same id as cached: nothing more ............................. UpdateSelectedItemIcon
//  70  :1472-1477 rows 0, id cached; no record or no icon resource: nothing more ... UpdateSelectedItemIcon
//  71  :1478-1484 the icon read from DBASE200 (size dword, then the bytes) .... ItemIconPicture (roth_uidraw, moved there from roth_invdraw)
//  72  :1485-1488 width, rows = height >> 1 (not hi-res) ...................... UpdateSelectedItemIcon
//  73  :1489-1490 decoded with mode 2 (every other row) into a buffer of pitch = width ... UpdateSelectedItemIcon
//  74  encode_item_icon_to_spans (INV:939-966): per row the leading zeros, then the run up to the last non-zero ... UpdateSelectedItemIcon
//  draw_held_item_icon, INV:1498-1521
//  75  nothing below a 0x46-row view; the slide: shown whole for 0x8c ticks, then down to half the rows (at least min(rows, 8)) ... DrawHeldItemIcon
//  76  at (view right - (width + 4), view bottom - shown) ...................... DrawHeldItemIcon
//  draw_translucent_icon_spans, B2:455-476
//  77  per row: index 0 before and after the run; the run's non-zero bytes through the blend table (selector 0x90be2 = 0x85d08) over the screen ... DrawHeldItemIcon, HudCanvas::BlendOverBehind
//  primitives (roth_uidraw)
//  78  draw_ui_panel_image_at_xy / _block (MH:127-143): blit_das_image_to_buffer mode 1 at base + x + y * pitch ... PanelImage (UiCanvas::BlitDasImageToBuffer, ebx = the pitch)
//  79  resolve_reloc_ptr (R:1247-1254) ...................................... UiCanvas::RelocImage
//  80  clear_buffer_rows (B2:58-66): rows top-tested ........................... ClearCells
//  81  draw_text_to_buffer, the ammunition count ........................... UiCanvas::DrawTextToBuffer
//
// What the original does to memory a port cannot: a picture larger than the
// stack work buffer, a copy reading work-buffer bytes the blit did not write,
// an icon larger than the held icon's buffers. Each is reported as a Problem
// (and the test fails on any), never approximated.
//

#include "roth_huddraw.h"
#include "roth_dbase100.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace roth
{

//==========================================================================
//
// The executable's tables, transcribed from the running original's bytes
// (tools/huddraw_test, the probe's "X" lines; the test compares them).
//
//==========================================================================

const uint32_t HudTables::health[21] = {
	0x50, 0xc, 0x28, 0x0, 0x280, 0x0, 0x0, 0x288, 0x290, 0x3, 0x1, 0x4a, 0x3, 0x1, 0x6, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1,
};
const uint32_t HudTables::weaponPanel[15][21] = {
	{ 0x66, 0x1c, 0x27, 0x2, 0x160, 0x0, 0x0, 0x168, 0x0, 0x1c, 0x6, 0x5, 0x9, 0x2, 0xa, 0x170, 0x3d, 0x8, 0x47, 0xb, 0x0 },
	{ 0x82, 0x13, 0x3c, 0x5, 0x1b0, 0x0, 0x0, 0x1b8, 0x0, 0x28, 0x6, 0x7, 0x9, 0x1, 0x0, 0x1c0, 0x55, 0x8, 0x69, 0x6, 0x0 },
	{ 0x82, 0x17, 0x40, 0x2, 0x198, 0x0, 0x0, 0x1a0, 0x1a8, 0x7, 0xc, 0x8, 0x10, 0x1, 0x7, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x6b, 0x17, 0x35, 0x3, 0x148, 0x0, 0x0, 0x150, 0x158, 0x16, 0x6, 0xb, 0xd, 0x1, 0xa, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x59, 0x1c, 0x2b, 0x2, 0x1c8, 0x0, 0x0, 0x1d0, 0x1d8, 0x17, 0x1, 0x30, 0x14, 0x1, 0x5, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1 },
	{ 0x5d, 0x19, 0x2d, 0x2, 0x178, 0x0, 0x0, 0x180, 0x188, 0x2, 0x1, 0x43, 0xa, 0x1, 0x5, 0x0, 0x190, 0x0, 0x0, 0x0, 0x2 },
	{ 0x8b, 0x11, 0x45, 0x6, 0x1e0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3 },
	{ 0x8b, 0x11, 0x45, 0x6, 0x1e0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3 },
	{ 0x8b, 0x11, 0x45, 0x6, 0x1e0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3 },
	{ 0x5a, 0x15, 0x2c, 0x4, 0x238, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3 },
	{ 0xf, 0x1b, 0x6, 0x2, 0x0, 0x0, 0x0, 0x228, 0x218, 0x0, 0x1, 0xd, 0x0, 0x1, 0x6, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0xf, 0x1b, 0x6, 0x2, 0x0, 0x0, 0x0, 0x388, 0x230, 0x0, 0x1, 0xd, 0x0, 0x1, 0x6, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x7f, 0x1a, 0x3e, 0x3, 0x1f8, 0x0, 0x0, 0x1e8, 0x1f0, 0x10, 0x6, 0xa, 0x10, 0x1, 0xa, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x79, 0x15, 0x3c, 0x7, 0x210, 0x0, 0x0, 0x200, 0x208, 0x17, 0x6, 0xc, 0x6, 0x1, 0xf, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x1f, 0x1d, 0xe, 0x1, 0x390, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x3 },
};
const uint32_t HudTables::weaponPanelAt[15] = {
	0x71420, 0x71474, 0x714c8, 0x7166c, 0x716c0, 0x71714, 0x71768, 0x71768, 0x71768, 0x71810, 0x715c4, 0x71618, 0x7151c, 0x71570, 0x717bc,
};
const uint8_t HudTables::messageColor = 0x67;
const char HudTables::ammoFormat[5] = "\x9e%D";
const uint8_t HudTables::edgeTL[4] = { 0x05, 0x0a, 0x0f, 0x15 };
const uint8_t HudTables::edgeBR[4] = { 0x15, 0x0f, 0x0a, 0x05 };
const uint8_t HudTables::cornerTL[16] = { 0x00, 0x01, 0x02, 0x04, 0x01, 0x03, 0x05, 0x08, 0x02, 0x05, 0x09, 0x0c, 0x04, 0x08, 0x0c, 0x10 };
const uint8_t HudTables::cornerTR[16] = { 0x04, 0x02, 0x01, 0x00, 0x08, 0x05, 0x03, 0x01, 0x0c, 0x09, 0x05, 0x02, 0x10, 0x0c, 0x08, 0x04 };
const uint8_t HudTables::cornerBL[16] = { 0x04, 0x08, 0x0c, 0x10, 0x02, 0x05, 0x09, 0x0c, 0x01, 0x03, 0x05, 0x08, 0x00, 0x01, 0x02, 0x04 };
const uint8_t HudTables::cornerBR[16] = { 0x10, 0x0c, 0x08, 0x04, 0x0c, 0x09, 0x05, 0x02, 0x08, 0x05, 0x03, 0x01, 0x04, 0x02, 0x01, 0x00 };

namespace
{

const int PITCH = HudPainter::SCREEN_W;
const int HEALTH_BYTES = 0x4b0;          // MH:453-455
const int WEAPON_BYTES = 0x1290;         // WC:1048-1049
const int WORK_PITCH = 0x64;             // uint32_t work[250] (MH:334, 380, 396, 577, 593): 10 rows of 0x64
const int WORK_ROWS = 10;
const size_t SPAN_BYTES = 0x6a0;         // g_held_item_icon_spans 0x81310, up to 0x819b0
const int ICONBUF_BYTES = 0x800;         // update_selected_item_icon's decode buffer [ebp-0x1004..-0x804]
const size_t ICONREAD_BYTES = 0x804;     // and the file's bytes read above it, to the frame's end

uint16_t U16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

} // namespace

//==========================================================================
//
// The pictures.
//
//==========================================================================

void HudCanvas::BlendOverBehind(long addr, uint8_t src)
{
	const long y = addr / VPITCH - VORG - (mHires ? (long)mTop * 2 : (long)mTop);
	const long x = addr % VPITCH - VORG - mLeft;
	if (x < 0 || x >= mW || y < 0 || y >= Height()) return;
	const size_t i = (size_t)(y * mW + x);
	if (mA.blend == nullptr) { Problem("no blend table in the map's pack: the held icon is not drawn"); return; }
	if (mMask[i] == 1) { mPix[i] = mA.blend[((size_t)src << 8) | mPix[i]]; mVersion++; }
	else if (mMask[i] == 0) { mPix[i] = src; mMask[i] = 2; mVersion++; }
	else Problem("a translucent pixel over a blended one (the engine shows one blend a pixel)");
}

void HudPainter::Bind(const HudAssets &assets)
{
	mA = assets;
	for (UiCanvas *c : { (UiCanvas *)&mHealthBuf, (UiCanvas *)&mWeaponBuf, (UiCanvas *)&mWork,
		(UiCanvas *)&mTop, (UiCanvas *)&mHeld, (UiCanvas *)&mTopOut, (UiCanvas *)&mHeldOut })
		c->SetAssets(mA.ui);
	mHealthBuf.SetRect(0, 0, 0, 0, false);
	mWeaponBuf.SetRect(0, 0, 0, 0, false);
	mWork.SetRect(0, 0, WORK_PITCH, WORK_ROWS, false);
	mHealthLive = mWeaponLive = false;
	mDesc = nullptr;
	mCellCounter = 0;
	mSpans.assign(SPAN_BYTES, 0);
	mHeldW = mHeldRows = mHeldId = 0;
	mTop.SetRect(0, 0, SCREEN_W, TOP_H, false);
	mHeld.SetRect(SCREEN_W - HELD_W, SCREEN_H - HELD_H, HELD_W, HELD_H, false);
	mTopOut = mTop;
	mHeldOut = mHeld;
	if (mA.ui.font == nullptr || !mA.ui.font->ok()) Problem("no font: the ammunition count is not drawn (ROTH.EXE's font was not found)");
}

void HudPainter::SetPackTables(const uint8_t *shade, const uint8_t *blend)
{
	mA.ui.shade = shade;
	mA.ui.blend = blend;
	for (UiCanvas *c : { (UiCanvas *)&mHealthBuf, (UiCanvas *)&mWeaponBuf, (UiCanvas *)&mWork,
		(UiCanvas *)&mTop, (UiCanvas *)&mHeld, (UiCanvas *)&mTopOut, (UiCanvas *)&mHeldOut })
		c->SetAssets(mA.ui);
}

void HudPainter::Problem(const std::string &s)
{
	for (const std::string &p : mProblems) if (p == s) return;
	mProblems.push_back(s);
}

std::vector<std::string> HudPainter::Problems() const
{
	std::vector<std::string> out = mProblems;
	for (const UiCanvas *c : { (const UiCanvas *)&mHealthBuf, (const UiCanvas *)&mWeaponBuf, (const UiCanvas *)&mWork,
		(const UiCanvas *)&mTop, (const UiCanvas *)&mHeld })
		for (const std::string &p : c->Problems())
			if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
	return out;
}

void HudPainter::ScreenPut(int x, int y, uint8_t v)
{
	mTop.Put(mTop.Addr(x, y), v);
	mHeld.Put(mHeld.Addr(x, y), v);
	if (y >= TOP_H && (x < SCREEN_W - HELD_W || y < SCREEN_H - HELD_H)) Problem("a HUD pixel outside both pictures");
}

void HudPainter::ScreenShade(int x, int y, int w, int rows, uint8_t row)
{
	mTop.ShadeRemapBlit(x, y, w, rows, row);
	mHeld.ShadeRemapBlit(x, y, w, rows, row);
}

void HudPainter::ComposeOnto(uint8_t *screen) const
{
	for (const HudCanvas *c : { &mTopOut, &mHeldOut })
	{
		const std::vector<uint8_t> &px = c->Pixels(), &mk = c->Mask();
		for (int y = 0; y < c->Height(); y++)
			for (int x = 0; x < c->Width(); x++)
			{
				const int sx = c->Left() + x, sy = c->Top() + y;
				if (sx < 0 || sx >= SCREEN_W || sy < 0 || sy >= SCREEN_H) continue;
				const size_t i = (size_t)y * c->Width() + x;
				uint8_t &d = screen[(size_t)sy * SCREEN_W + sx];
				const uint8_t m = mk[i];
				if (m == 1) d = px[i];
				else if (m == 2 && mA.ui.blend != nullptr) d = mA.ui.blend[((size_t)px[i] << 8) | d];
				else if (m >= 0x20 && mA.ui.shade != nullptr) d = mA.ui.shade[((size_t)(m - 0x20) << 8) | d];
			}
	}
}

//==========================================================================
//
// Helpers.
//
//==========================================================================

// draw_ui_panel_image_at_xy (MH:127-133) / draw_ui_panel_image_block
// (:138-143): blit_das_image_to_buffer(img, base + x + y * pitch, pitch, 1).
void HudPainter::PanelImage(UiCanvas &buf, uint32_t pitch, uint32_t id, int x, int y)
{
	size_t avail = 0;
	const uint8_t *img = buf.RelocImage(id, &avail);
	if (img == nullptr || avail < 8) return;
	// The buffer is linear in the original: a picture past its pitch would
	// wrap into the next row there and is cut off here.
	if (x < 0 || x + (int)U16(img + 4) > (int)pitch) Problem("a HUD picture wider than its panel buffer (it would wrap in the original)");
	buf.BlitDasImageToBuffer(img, avail, buf.Addr(x, y), pitch, 1);
}

void HudPainter::WorkRows(uint32_t id, UiCanvas &buf, int x, int y, uint32_t from, uint32_t count, uint32_t rows)
{
	size_t avail = 0;
	const uint8_t *img = mWork.RelocImage(id, &avail);
	if (img == nullptr || avail < 8) return;
	if ((int)U16(img + 4) > WORK_PITCH || (int)U16(img + 6) > WORK_ROWS) Problem("a HUD picture larger than the original's stack work buffer");
	mWork.SetRect(0, 0, WORK_PITCH, WORK_ROWS, false);                      // what this blit writes, known by the mask
	mWork.BlitDasImageToBuffer(img, avail, mWork.Addr(0, 0), WORK_PITCH, 1);
	for (uint32_t r = 0; r < rows; r++)
		for (uint32_t k = 0; k < count; k++)
		{
			uint8_t v = 0;
			if (from + k >= (uint32_t)WORK_PITCH || !mWork.Get(mWork.Addr((int)(from + k), (int)r), v))
				Problem("a HUD copy reads work-buffer bytes its picture did not write (the original's stack holds them)");
			buf.Put(buf.Addr(x + (int)k, y + (int)r), v);
		}
	if (x + (int)count > buf.Width()) Problem("a HUD bar wider than its panel buffer (it would wrap in the original)");
}

// clear_buffer_rows (B2:58-66): rows top-tested.
void HudPainter::ClearCells(UiCanvas &buf, int x, int y, int w, int h)
{
	if (x + w > buf.Width()) Problem("a cleared cell wider than its panel buffer (it would wrap in the original)");
	for (int r = 0; r < h; r++)
		for (int k = 0; k < w; k++) buf.Put(buf.Addr(x + k, y + r), 0);
}

//==========================================================================
//
// The health panel.
//
//==========================================================================

void HudPainter::RefreshHudLayout(int32_t health, int32_t maxHealth)
{
	mHealthLive = false;                                                    // :428 free_hud_weapon_das_handle
	// :431-433, full view: (1, 8). render_health_status_panel puts the 8 at
	// +0x44, which blit_panel_image reads as the column (B2:1407).
	mHealthX = 8;
	mHealthLive = true;                                                     // :453-456
	RenderHealthStatusPanel(health, maxHealth);                             // :457-458
}

void HudPainter::RenderHealthStatusPanel(int32_t health, int32_t maxHealth)
{
	const uint32_t *d = HudTables::health;
	mHealthBuf.SetRect(0, 0, (int)d[0], HEALTH_BYTES / (int)d[0], false);   // :311, the buffer cleared
	if (d[4] != 0) PanelImage(mHealthBuf, d[0], d[4], (int)d[5], (int)d[6]); // :313-314
	uint32_t img = d[7];                                                    // :316-318
	uint32_t h = (uint32_t)health;                                          // :320-323
	if ((int32_t)h < 0) h = 0;
	const uint32_t maxh = (uint32_t)maxHealth;
	if (h > maxh) h = maxh;
	uint32_t fill = 0;                                                      // :325-328
	if (maxh != 0) fill = (d[11] * h) / maxh;
	if (fill == 0 && h != 0) fill = 1;
	if (fill < (d[11] >> 1)) img = 0x298;                                   // :330-331
	if (fill != 0) WorkRows(img, mHealthBuf, (int)d[9], (int)d[12], 0, fill, d[14]);   // :333-343
}

void HudPainter::RenderPlayerHealthBar(int32_t health, int32_t maxHealth)
{
	if (onRoutine) onRoutine("render_player_health_bar", 0);
	const uint32_t *d = HudTables::health;
	g_health_bar_dirty = 0;                                                 // :362
	uint32_t h = (uint32_t)health;                                          // :365-369
	if ((int32_t)h < 0) h = 0;
	if (h > (uint32_t)maxHealth) h = (uint32_t)maxHealth;
	if (maxHealth == 0) { Problem("the maximum health is 0 (the original divides by it here)"); return; }
	uint32_t fill = (d[11] * h) / (uint32_t)maxHealth;                      // :371-372
	if (fill == 0 && h != 0) fill = 1;
	if (fill != 0) WorkRows(fill < (d[11] >> 1) ? 0x298 : d[7], mHealthBuf, (int)d[9], (int)d[12], 0, fill, d[14]);   // :376-389
	const uint32_t empty = d[11] - fill;                                    // :391-405
	if (empty != 0) WorkRows(d[8], mHealthBuf, (int)(d[9] + fill), (int)d[12], fill, empty, d[14]);
}

//==========================================================================
//
// The weapon panel.
//
//==========================================================================

int32_t HudPainter::RenderWeaponHud(const HudWeapon &w)
{
	if (onRoutine) onRoutine("render_weapon_hud", 1);
	mDesc = nullptr;                                                        // WC:998
	int32_t panel = w.panel;
	if ((uint32_t)panel > 0xf) panel = 0;                                   // :1015-1016
	mWeaponLive = false;                                                    // :1017-1026 (both panel buffers freed)
	if (panel == 0) return panel;
	mDesc = HudTables::weaponPanel[panel - 1];                              // :1048-1053
	mWeaponBuf.SetRect(0, 0, (int)mDesc[0], WEAPON_BYTES / (int)mDesc[0], false);
	mWeaponLive = true;
	RenderUiTexturePanel(w);                                                // :1054
	return panel;
}

void HudPainter::RenderUiTexturePanel(const HudWeapon &w)
{
	if (mDesc == nullptr || !mWeaponLive) return;
	const uint32_t *d = mDesc;
	g_weapon_panel_dirty = 0;                                               // MH:528
	mWeaponX = (int)((uint32_t)PITCH >> 1) - (int)d[2];                     // :532-536, full view
	mWeaponBuf.SetRect(0, 0, (int)d[0], WEAPON_BYTES / (int)d[0], false);   // :544, the buffer cleared
	if (d[4] != 0) PanelImage(mWeaponBuf, d[0], d[4], (int)d[5], (int)d[6]);   // :546-547
	uint32_t img = d[7];                                                    // :549-550
	const uint32_t alt = d[8];
	int32_t item = w.item;                                                  // :551-554
	const int32_t x0 = (int32_t)d[9];
	int32_t y0 = (int32_t)d[12];
	const uint32_t mode = d[20];
	if (mode == 0)                                                          // :556-572
	{
		if ((uint32_t)w.ammoCap > 2) item = item >> 8;
		mCellCounter = item;
		for (uint32_t r = 0; r < d[13]; r++)
		{
			int32_t x = x0;
			for (uint32_t c = 0; c < d[10]; c++)
			{
				item--;
				if (item < 0)
				{
					if (alt == 0) break;
					img = alt;
				}
				PanelImage(mWeaponBuf, d[0], img, x, y0);
				x += (int32_t)d[11];
			}
			y0 += (int32_t)d[14];
		}
	}
	else if (mode == 1 || mode == 2)                                        // :573-602
	{
		if (item > 0x100) item = 0x100;
		if (item < 0) { Problem("a negative charge (the original copies past its buffers here)"); item = 0; }
		const uint32_t wfill = (d[11] * (uint32_t)item) >> 8;
		const uint32_t rem = d[11] - wfill;
		if (wfill != 0)
		{
			if (mode == 1) WorkRows(img, mWeaponBuf, (int)d[9], (int)d[12], 0, wfill, d[14]);
			else WorkRows(img, mWeaponBuf, (int)(d[9] + rem), (int)d[12], rem, wfill, d[14]);
		}
	}
	if ((uint32_t)w.ammo != 0)                                              // :605-616
	{
		if (d[15] != 0) PanelImage(mWeaponBuf, d[0], d[15], (int)d[16], (int)d[17]);
		if (d[18] != 0 && (uint32_t)w.ammo > 1)
		{
			uint8_t sb[16] = {};
			char num[12];
			snprintf(num, sizeof num, "%d", (int)w.ammo);                  // "\x9e%D": Watcom's %D, a signed decimal
			sb[0] = 1;
			sb[1] = HudTables::messageColor;
			sb[2] = (uint8_t)HudTables::ammoFormat[0];
			memcpy(sb + 3, num, strlen(num) + 1);
			const int tw = mWeaponBuf.MeasureControlTextWidth(sb, sizeof sb);
			if ((int)d[18] + tw + 2 > (int)d[0]) Problem("the ammunition count wider than its panel (it would wrap in the original)");
			mWeaponBuf.DrawTextToBuffer(sb, sizeof sb, mWeaponBuf.Addr((int)d[18], (int)d[19]), 0);
		}
	}
}

void HudPainter::RenderWeaponView(int arg, const HudWeapon &w)
{
	if (onRoutine) onRoutine("render_weapon_view", arg);
	if (mDesc == nullptr || !mWeaponLive) return;                           // WC:1228-1229
	const uint32_t *d = mDesc;
	const uint32_t mode = d[20];
	if (mode == 1 || mode == 2)                                             // :1237-1260
	{
		uint32_t v = (uint32_t)w.item;
		if (v > 0x100) v = 0x100;
		const uint32_t scaled = (uint32_t)((int32_t)d[11] * (int32_t)v) >> 8;
		const uint32_t rem = d[11] - scaled;
		const int x = (int)d[9], y = (int)d[12];
		if (mode == 1)
		{
			if (scaled != 0) WorkRows(d[7], mWeaponBuf, x, y, 0, scaled, d[14]);
			if (rem != 0) WorkRows(d[8], mWeaponBuf, x + (int)scaled, y, scaled, rem, d[14]);
		}
		else
		{
			if (rem != 0) WorkRows(d[8], mWeaponBuf, x, y, 0, rem, d[14]);
			if (scaled != 0) WorkRows(d[7], mWeaponBuf, x + (int)scaled, y, scaled, scaled, d[14]);
		}
		return;
	}
	if (mode != 0) return;                                                  // :1343-1348 (mode 3: nothing)
	int32_t boxX = (int32_t)d[9], boxY = (int32_t)d[12];
	if (arg <= 0)                                                           // :1262-1310
	{
		int32_t idx = mCellCounter - 1;
		mCellCounter = idx;
		if (idx < 0) return;
		if (idx >= (int32_t)d[10])
		{
			idx -= (int32_t)d[10];
			if (idx >= (int32_t)d[10]) return;
			if (d[13] == 1) return;
			boxY += (int32_t)d[14];
		}
		boxX += (int32_t)d[11] * idx;
		if (d[8] != 0) PanelImage(mWeaponBuf, d[0], d[8], boxX, boxY);
		else
		{
			size_t avail = 0;
			const uint8_t *img = mWeaponBuf.RelocImage(d[7], &avail);
			if (img == nullptr || avail < 8) return;
			ClearCells(mWeaponBuf, boxX, boxY, (int16_t)U16(img + 4), (int16_t)U16(img + 6));
		}
		return;
	}
	if (arg != 1) return;                                                   // :1342
	int32_t val = w.item;                                                   // :1311-1341
	if (w.ammoCap > 2) val >>= 8;
	size_t digitSize = 0;
	const uint8_t *digit = d[7] != 0 ? mWeaponBuf.RelocImage(d[7], &digitSize) : nullptr;
	mCellCounter = val;
	for (int32_t row = 0; row < (int32_t)d[13]; row++)
	{
		int32_t cx = boxX;
		for (int32_t col = 0; col < (int32_t)d[10]; col++)
		{
			val--;
			if (val >= 0) PanelImage(mWeaponBuf, d[0], d[7], cx, boxY);
			else if (d[8] == 0)
			{
				if (digit != nullptr && digitSize >= 8) ClearCells(mWeaponBuf, cx, boxY, (int16_t)U16(digit + 4), (int16_t)U16(digit + 6));
			}
			else PanelImage(mWeaponBuf, d[0], d[8], cx, boxY);
			cx += (int32_t)d[11];
		}
		boxY += (int32_t)d[14];
	}
}

void HudPainter::RedrawWeaponHudPanel(const HudWeapon &w)
{
	if (onRoutine) onRoutine("redraw_weapon_hud_panel", 0);
	if (mWeaponLive) RenderUiTexturePanel(w);                               // WC:542-552
}

//==========================================================================
//
// The frame.
//
//==========================================================================

void HudPainter::DrawActiveUiPanels(int32_t health, int32_t maxHealth, const HudWeapon &w)
{
	// copy_nonzero_bytes (R:9651-9660): from count - 1 down to 0, nothing when count <= 1.
	auto copyNonzero = [&](int x, int y, const uint8_t *src, uint32_t count) {
		int32_t i = (int32_t)count - 1;
		if (i <= 0) return;
		do { if (src[i]) ScreenPut(x + i, y, src[i]); } while (--i >= 0);
	};
	if (mWeaponLive && mDesc != nullptr)                                    // MH:752
	{
		if (g_weapon_panel_dirty != 0) RenderUiTexturePanel(w);             // :753-754
		// render_ui_panel_text, full view (:664-686): width d[0] (its +0x1c),
		// rows d[1] + 3 (+0x20), from row 1 at the panel's column.
		const uint32_t width = mDesc[0], rows = mDesc[1] + 3;
		for (uint32_t r = 0; r < rows && (int)r < mWeaponBuf.Height(); r++)
			copyNonzero(mWeaponX, 1 + (int)r, mWeaponBuf.Pixels().data() + (size_t)r * width, width);
	}
	if (mHealthLive)                                                        // :768
	{
		if (g_health_bar_dirty != 0) RenderPlayerHealthBar(health, maxHealth);   // :770-771
		// blit_panel_image, full view (B2:1405-1428): stride d[0] (+0x18), rows d[1] (+0x1c).
		const uint32_t *d = HudTables::health;
		for (uint32_t r = 0; r < d[1]; r++)
			copyNonzero(mHealthX, 1 + (int)r, mHealthBuf.Pixels().data() + (size_t)r * d[0], d[0]);
	}
}

void HudPainter::CornerPeekTimer(bool inCorner, int fts)
{
	if (inCorner)                                                           // GC:520-526
	{
		g_corner_peek += fts;
		if (g_corner_peek > 0x19) g_corner_peek = 0x19;
		return;
	}
	if (g_corner_peek == 0) return;                                         // :528-533
	g_corner_peek -= fts;
	if (g_corner_peek > 0) return;
	g_corner_peek = 0;                                                      // clear_corner_peek_icon (MH:234)
}

// draw_shadow_border_edge_h (B2:159-195): row i of `count` pixels through gradient src[i].
void HudPainter::ShadowEdgeH(int x, int y, int count, const uint8_t *src)
{
	int w = PITCH - x;
	if (w >= 4) w = 4;
	for (int i = 0; i < w; i++)
	{
		if ((long)(y + i) * PITCH + x < 0) continue;
		ScreenShade(x, y + i, count, 1, src[i]);                            // bottom-tested: at least one pixel
	}
}

// draw_shadow_border_edge_v (B2:205-224): column i of `height` rows through src[i].
void HudPainter::ShadowEdgeV(int x, int y, int height, const uint8_t *src)
{
	for (int i = 0; i < 4; i++)
	{
		if ((long)y * PITCH + x + i < 0) continue;
		ScreenShade(x + i, y, 1, height, src[i]);
	}
}

// blit_translucent_overlay_block (B2:108-150).
void HudPainter::OverlayBlock(int x, int y, const uint8_t *src)
{
	for (int r = 0; r < 4; r++)
	{
		if ((uint32_t)(y + r) >= (uint32_t)SCREEN_H) continue;
		for (int k = 0; k < 4; k++)
			if ((uint32_t)(x + k) < (uint32_t)PITCH) ScreenShade(x + k, y + r, 1, 1, src[r * 4 + k]);
	}
}

void HudPainter::DrawPopupShadowBorder(int x, int y, int w, int h)
{
	if (y > 0) ShadowEdgeH(x, y - 4, w, HudTables::edgeTL);                 // B2:1996-2003
	if (y + h < SCREEN_H) ShadowEdgeH(x, y + h, w, HudTables::edgeBR);
	if (x > 0) ShadowEdgeV(x - 4, y, h, HudTables::edgeTL);
	if (x + w < PITCH) ShadowEdgeV(x + w, y, h, HudTables::edgeBR);
	OverlayBlock(x - 4, y - 4, HudTables::cornerTL);                       // :2005-2008
	OverlayBlock(x - 4, y + h, HudTables::cornerBL);
	OverlayBlock(x + w, y + h, HudTables::cornerBR);
	OverlayBlock(x + w, y - 4, HudTables::cornerTR);
}

void HudPainter::DrawCharacterPortraitCorner(int32_t t, int32_t adamItemId)
{
	int bx = 4, size = 0x1c;
	if (t < 10) { bx = 0x12 - (t + 4); size = (t + 4) * 2; }                // MH:272-276
	DrawPopupShadowBorder(bx, bx, size, size);                              // :275, 278
	mTop.ClearFramebufferRect(bx, bx, size, size);                          // :276, 279 (the top strip holds the corner)
	if (t < 10) return;
	const uint8_t *icon = nullptr; size_t isz = 0;                          // :280-285, [0x7fed0]
	if (mA.db == nullptr || mA.dbase200 == nullptr || !ItemIconPicture(*mA.db, *mA.dbase200, adamItemId, &icon, &isz)) return;
	const int32_t s = t - 10;
	const uint32_t tint = (s < 0xe) ? (uint32_t)((0xf - s) << 9) : 0u;
	// draw_item_icon_centered (INV:1202-1211) at (4, 4).
	const int x = 4 + ((0x1c - (int)(int16_t)U16(icon + 4)) >> 1);
	const int y = 4 + ((0x38 - (int)(int16_t)U16(icon + 6)) >> 2);
	if (y < 0) Problem("Adam's icon above the screen");
	mTop.BlitItemIcon(icon, isz, x, y, (tint << 8) | 2u);
}

void HudPainter::UpdateSelectedItemIcon(int32_t itemId)
{
	if (onRoutine) onRoutine("update_selected_item_icon", 0);
	g_held_slide = 0;                                                       // INV:1464
	if (itemId == 0 || g_held_icon_enabled == 0)                            // :1465-1468
	{
		mHeldRows = 0;
		mHeldId = 0;
		return;
	}
	if (itemId == mHeldId) return;                                          // :1471
	mHeldRows = 0;                                                          // :1472-1473
	mHeldId = itemId;
	const uint8_t *icon = nullptr; size_t size = 0;                         // :1474-1484
	if (mA.db == nullptr || mA.dbase200 == nullptr || !ItemIconPicture(*mA.db, *mA.dbase200, itemId, &icon, &size)) return;
	if (size > ICONREAD_BYTES) { Problem("an item icon larger than update_selected_item_icon's stack buffer"); return; }
	const int32_t w = (int16_t)U16(icon + 4), h = (int16_t)U16(icon + 6);  // :1485-1487
	mHeldW = w;
	mHeldRows = (int32_t)((uint32_t)h >> 1);
	if (w <= 0 || mHeldRows <= 0) { Problem("an item icon too small for the held icon's encoder (the original loops)"); mHeldRows = 0; return; }
	const int decodedRows = (icon[0] & 1) ? (h + 1) / 2 : h;               // mode 2: every other row (run-length pictures only)
	if ((long)decodedRows * w > ICONBUF_BYTES) { Problem("an item icon larger than update_selected_item_icon's decode buffer"); mHeldRows = 0; return; }
	UiCanvas buf;                                                           // :1489-1490, pitch = width
	buf.SetRect(0, 0, w, ICONBUF_BYTES / w, false);
	buf.BlitDasImageToBuffer(icon, size, buf.Addr(0, 0), (uint32_t)w, 2);
	for (const std::string &p : buf.Problems()) Problem(p);
	// encode_item_icon_to_spans (INV:939-966).
	size_t o = 0;
	const uint8_t *src = buf.Pixels().data();
	for (int32_t r = 0; r < mHeldRows; r++, src += w)
	{
		int32_t skip = 0;
		while (skip < w && src[skip] == 0) skip++;
		int32_t last = -1;
		for (int32_t k = skip; k < w; k++) if (src[k] != 0) last = k - skip;
		const int32_t run = skip < w ? last + 1 : 0;
		if (o + 2 + (size_t)run > SPAN_BYTES) { Problem("an item icon larger than the held icon's span buffer"); mHeldRows = 0; return; }
		mSpans[o] = (uint8_t)skip;
		mSpans[o + 1] = (uint8_t)run;
		memcpy(mSpans.data() + o + 2, src + skip, (size_t)run);
		o += 2 + (size_t)run;
	}
}

void HudPainter::DrawHeldItemIcon(int fts)
{
	if (SCREEN_H < 0x46) return;                                            // INV:1500
	const int32_t edx = g_held_slide - 0x8c;                                // :1501-1515
	int32_t ecx = mHeldRows;
	int32_t eax = ecx + 4;
	if (edx > 0)
	{
		eax -= edx;
		int32_t d = ecx >> 1;
		if (d < 8) { d = ecx; if (ecx > 8) d = 8; }
		if (eax < d) eax = d;
		else g_held_slide += fts;
		if ((uint32_t)ecx > (uint32_t)eax) ecx = eax;
	}
	else g_held_slide += fts;
	if (eax <= 0 || ecx == 0) return;                                       // :1516
	const int32_t y = SCREEN_H - eax;                                       // :1517-1518, the view = the screen
	const int32_t x = SCREEN_W - (mHeldW + 4);
	if (x < SCREEN_W - HELD_W || y < SCREEN_H - HELD_H) Problem("the held icon larger than its picture");
	// draw_translucent_icon_spans (B2:455-476).
	size_t si = 0;
	int32_t rows = ecx, yy = y;
	do {
		if (si + 2 > mSpans.size()) break;
		int dx = x + mSpans[si];
		int32_t run = mSpans[si + 1];
		si += 2;
		if (run != 0)
		{
			ScreenPut(dx++, yy, 0);
			do {
				const uint8_t s = si < mSpans.size() ? mSpans[si] : 0;
				si++;
				if (s != 0) mHeld.BlendOverBehind(mHeld.Addr(dx, yy), s);
				dx++;
			} while (--run > 0);
			ScreenPut(dx, yy, 0);
		}
		yy++;
	} while (--rows > 0);
}

void HudPainter::DrawFrame(int fts, int32_t health, int32_t maxHealth, const std::function<HudWeapon()> &w, int32_t adamItemId,
	const std::function<void()> &tick)
{
	mTop.SetRect(0, 0, SCREEN_W, TOP_H, false);
	mHeld.SetRect(SCREEN_W - HELD_W, SCREEN_H - HELD_H, HELD_W, HELD_H, false);
	DrawHeldItemIcon(fts);                                                  // GC:494
	if (tick) tick();                                                       // :495, draw_active_ui_panels' first call (MH:751)
	DrawActiveUiPanels(health, maxHealth, w());
	if (g_corner_peek != 0) DrawCharacterPortraitCorner(g_corner_peek, adamItemId);   // :498-499
	if (!mTop.SameAs(mTopOut)) mTopOut = mTop;
	if (!mHeld.SameAs(mHeldOut)) mHeldOut = mHeld;
}

} // namespace roth
