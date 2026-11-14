#pragma once
//
// Realms of the Haunting's HUD, drawn as the original draws it: the health
// panel (top left), the weapon panel with its ammunition or charge (top
// centre), the corner peek of Adam's item (top left, while the pointer rests
// in that corner) and the selected item's translucent icon (bottom right),
// from the install's own pictures (DATA\ICONS.ALL, DBASE200.DAT, the map
// pack's shade and blend tables, ROTH.EXE's font), placed by ports of the
// original's own routines. Engine-free: the C++ standard library and the
// readers it names, nothing else.
//
// The original draws all of it into its 640 x 480 framebuffer, over the 3D
// view it has just rendered, once a frame (gameplay_frame_step,
// game_core.c:486-506). This module keeps the original's two panel buffers
// and redraws them when the original does; every frame it draws them, the
// corner and the held icon onto two pictures of the original's screen
// (HudCanvas, a UiCanvas of roth_uidraw): TopStrip(), the top 0x28 rows, and
// HeldIcon(), the bottom right corner. A pixel's mask says what it is:
//   0          untouched: the 3D view shows through
//   1          a palette index drawn over the view (opaque; index 0 included:
//              the corner's black box and the held icon's outline are index 0)
//   0x20 + r   the view's own pixel through row r of the shade tables (the
//              corner's shadow; UiCanvas::ShadeRemapBlit's convention)
//   2          the view's own pixel through the blend table, the picture's
//              index the source (the held icon, draw_translucent_icon_spans)
// ComposeOnto() gives the exact screen over a known view (the test); the
// engine draws the masks over its rendered view (roth_interface.cpp).
//
// WHERE IT COMES FROM (ROTH.C, roth_c/src/engine); the numbered checklist
// with file:line on both sides is in roth_huddraw.cpp.
//   refresh_hud_layout              menu_hud_ui.c:426-459
//   render_health_status_panel      menu_hud_ui.c:299-349
//   render_player_health_bar        menu_hud_ui.c:357-418
//   render_weapon_hud               weapon_combat.c:994-1058
//   render_ui_texture_panel         menu_hud_ui.c:523-622
//   render_weapon_view              weapon_combat.c:1224-1369
//   redraw_weapon_hud_panel         weapon_combat.c:540-553
//   draw_active_ui_panels           menu_hud_ui.c:749-775
//   render_ui_panel_text            menu_hud_ui.c:658-686   (the full-view copy)
//   blit_panel_image                blit_2d.c:1402-1429     (the full-view copy)
//   draw_character_portrait_corner  menu_hud_ui.c:262-288
//   draw_popup_shadow_border_smc    blit_2d.c:1983-2008 (+ its edge and block blitters, :108-235)
//   clear_framebuffer_rect          renderer.c:1260-1271
//   draw_item_icon_centered         inventory.c:1202-1211
//   update_selected_item_icon       inventory.c:1461-1493
//   encode_item_icon_to_spans       inventory.c:939-966
//   draw_held_item_icon             inventory.c:1498-1521
//   draw_translucent_icon_spans     blit_2d.c:455-476
//   run_gameplay_frame's corner timer, clear_corner_peek_icon   game_core.c:520-533, menu_hud_ui.c:232-241
//
// Only the full-view layout is ported: the original's view filling its whole
// screen (g_view_h == the screen height), as in its retail VideoMode 8 and in
// every frame of it we have; the engine's view is always whole.
//

#include "roth_uidraw.h"

#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <string>
#include <vector>

namespace roth
{

class Dbase100;

// The executable's tables the HUD reads, transcribed from the running
// original's bytes (tools/huddraw_test, the probe's "X" lines; the test
// compares every one).
struct HudTables
{
	// The health panel's descriptor, 0x71864 (refresh_hud_layout passes
	// g_choice_selected_index + 0x4f4): 21 dwords, see the field list below.
	static const uint32_t health[21];
	// The weapon panels' descriptors: the 15 pointers at 0x718b8 (indexed by
	// a weapon's HUD panel number - 1, WC:1052) and the 21 dwords each points
	// at. weaponPanelAt is the executable address each pointer holds.
	static const uint32_t weaponPanel[15][21];
	static const uint32_t weaponPanelAt[15];
	// g_default_message_color 0x7675e: the ammunition count's colour.
	static const uint8_t messageColor;
	// 0x75ef0: the count's format, "\x9e%D".
	static const char ammoFormat[5];
	// The shadow's gradients (draw_popup_shadow_border_smc): 0x708ec the
	// top and left edges, 0x708f0 the bottom and right, then the four 4 x 4
	// corners, 0x708f4 top left, 0x70904 top right, 0x70914 bottom left,
	// 0x70924 bottom right.
	static const uint8_t edgeTL[4], edgeBR[4];
	static const uint8_t cornerTL[16], cornerTR[16], cornerBL[16], cornerBR[16];
};
// A descriptor's dwords, as the routines read them:
//   [0] buffer pitch  [1] rows  [2] half width (centring)  [3] y offset
//   [4] panel picture  [5],[6] its x, y  [7] bar / cell picture  [8] its empty / blank picture
//   [9] bar x  [10] cells across  [11] bar width / cell step  [12] bar y  [13] cell rows  [14] bar rows / row step
//   [15] ammo picture  [16],[17] its x, y  [18],[19] the count's x, y  [20] mode (0 cells, 1 fill, 2 fill from the right, 3 none)

struct HudAssets
{
	UiAssets ui;                                        // ICONS.ALL, the pack's shade and blend tables, the font
	const std::vector<uint8_t> *dbase200 = nullptr;     // DBASE200.DAT, whole (the item icons)
	const Dbase100 *db = nullptr;
};

// What the HUD's routines read of the weapon: the original's globals.
struct HudWeapon
{
	int32_t item = 0;        // g_active_weapon_item_id 0x83e70: the held slot's quantity word (rounds, or charge << 8)
	int32_t ammo = 0;        // g_active_weapon_ammo 0x83e6c: the clips carried
	int32_t ammoCap = 1;     // g_active_weapon_ammo_cap 0x83e74: 1, or 0x100 for a recharging weapon
	int32_t panel = 0;       // g_active_weapon_attrs + 0x48: the weapon's HUD panel number (0 none)
};

// A picture of part of the original's screen, with the HUD's two kinds of
// pixel over what lay behind.
class HudCanvas : public UiCanvas
{
public:
	// draw_translucent_icon_spans' pixel (blit_2d.c:466-469):
	// blend[(src << 8) | what is there].
	void BlendOverBehind(long addr, uint8_t src);
	bool SameAs(const HudCanvas &o) const { return mPix == o.mPix && mMask == o.mMask; }
};

class HudPainter
{
public:
	static const int SCREEN_W = 640, SCREEN_H = 480;   // the retail ROTH.INI's VideoMode 8; the view fills it
	// The two pictures: the top strip, and the bottom right corner the
	// selected item's icon slides in at (wide and tall enough for every item
	// icon of DBASE200; a write past it is a Problem).
	static const int TOP_H = 0x28;
	static const int HELD_W = 0x80, HELD_H = 0x60;

	void Bind(const HudAssets &assets);
	// The map's pack tables, when a map opens.
	void SetPackTables(const uint8_t *shade, const uint8_t *blend);

	// ---- the original's routines, each by its name ----------------------------
	// refresh_hud_layout (with render_health_status_panel): the health panel
	// rebuilt (repaint_hud_and_present: the game's start, a load, the
	// inventory closing).
	void RefreshHudLayout(int32_t health, int32_t maxHealth);
	// render_weapon_hud(1, attrs): the weapon panel for `w.panel`. A panel
	// number above 0xf is set to 0 in the weapon's table by the original
	// (WC:1015-1016): the return value is what the caller writes back.
	int32_t RenderWeaponHud(const HudWeapon &w);
	// render_weapon_view(arg): one round spent (0), the charge or the cells
	// redrawn (1).
	void RenderWeaponView(int arg, const HudWeapon &w);
	// redraw_weapon_hud_panel: the whole weapon panel again (a reload).
	void RedrawWeaponHudPanel(const HudWeapon &w);
	// update_selected_item_icon for the right hand's item (0: empty hand).
	void UpdateSelectedItemIcon(int32_t itemId);

	// ---- the globals other code writes ----------------------------------------
	int32_t g_health_bar_dirty = 0;     // 0x83e7c: set by every change to the health (WC:32, R:9820, R:10015, game_core.c:838)
	int32_t g_weapon_panel_dirty = 0;   // 0x83e80: set when the active ammunition is given or taken (inventory.c:265, 440)
	int32_t g_corner_peek = 0;          // 0x7fdc0: the corner peek's timer, 0..0x19
	int32_t g_held_slide = 0;           // 0x819bc: the held icon's slide timer (inventory.c:486, 1411 reset it)
	int32_t g_held_icon_enabled = 0;    // 0x819b8: set to 1 by the game's start (game_core.c:841)

	// ---- one frame ----------------------------------------------------------------
	// run_gameplay_frame's corner timer (game_core.c:520-533): `inCorner` is
	// the pointer within the top left 0x20 x 0x20 pixels.
	void CornerPeekTimer(bool inCorner, int fts);
	// gameplay_frame_step's drawing (game_core.c:494-500): draw_held_item_icon,
	// draw_active_ui_panels and, while the corner peek runs,
	// draw_character_portrait_corner, onto fresh pictures; they replace
	// TopStrip() / HeldIcon() (and move their Version()) only when they differ.
	// draw_active_ui_panels' first call, tick_weapon_hud_ammo_anim, is the
	// weapon logic's (ActorWorld::RechargeFrame): `tick`, called at that
	// point when given; its redraws come through RenderWeaponView(1). `w` is
	// asked for after it (the tick changes the shown charge).
	void DrawFrame(int fts, int32_t health, int32_t maxHealth, const std::function<HudWeapon()> &w, int32_t adamItemId,
		const std::function<void()> &tick = nullptr);

	// ---- the pictures ---------------------------------------------------------------
	// Where blit_panel_image puts the health panel on the screen (full view,
	// B2:1405-1428, MH:772-773): column mHealthX (8), row 1, d[0] x d[1]
	// (0x50 x 0xc). False before refresh_hud_layout has built it. For the VR
	// off-hand mount (roth_interface.cpp), which shows that part of TopStrip().
	bool HealthPanelRect(int &x, int &y, int &w, int &h) const
	{
		if (!mHealthLive) return false;
		x = mHealthX; y = 1; w = (int)HudTables::health[0]; h = (int)HudTables::health[1];
		return true;
	}
	// Whether render_health_status_panel / render_player_health_bar draw the
	// bar with ICONS.ALL 0x298, the "low" picture (MH:320-331, 363-389): the
	// fill d[11] * health / max (1 when that is 0 with any health) under half
	// of d[11]. For the VR low-health cue, which stands in for that picture.
	static bool HealthBarLow(int32_t health, int32_t maxHealth)
	{
		const uint32_t *d = HudTables::health;
		uint32_t h = (uint32_t)health;
		if ((int32_t)h < 0) h = 0;
		const uint32_t maxh = (uint32_t)maxHealth;
		if (h > maxh) h = maxh;
		if (maxh == 0) return false;
		uint32_t fill = (d[11] * h) / maxh;
		if (fill == 0 && h != 0) fill = 1;
		return fill < (d[11] >> 1);
	}
	const HudCanvas &TopStrip() const { return mTopOut; }
	const HudCanvas &HeldIcon() const { return mHeldOut; }
	// The original's screen: `screen` (SCREEN_W x SCREEN_H) holds the view the
	// HUD is drawn over; the last frame's pictures are drawn onto it.
	void ComposeOnto(uint8_t *screen) const;

	// What went wrong, if anything: one line each, the pictures' own included.
	std::vector<std::string> Problems() const;

	// For the test: the weapon panel's cell counter (0x83d7c) and the held
	// icon's cached id (0x8130c).
	int32_t CellCounter() const { return mCellCounter; }
	// Called on entry to each routine the original's logic reaches from
	// outside the HUD (render_weapon_hud, render_weapon_view,
	// redraw_weapon_hud_panel, update_selected_item_icon,
	// render_player_health_bar), with its argument: the test's trace.
	std::function<void(const char *routine, int arg)> onRoutine;
	int32_t HeldIconId() const { return mHeldId; }

private:
	// ---- the routines ----------------------------------------------------------
	void RenderHealthStatusPanel(int32_t health, int32_t maxHealth);
	void RenderPlayerHealthBar(int32_t health, int32_t maxHealth);
	void RenderUiTexturePanel(const HudWeapon &w);
	void DrawActiveUiPanels(int32_t health, int32_t maxHealth, const HudWeapon &w);
	void DrawHeldItemIcon(int fts);
	void DrawCharacterPortraitCorner(int32_t t, int32_t adamItemId);
	void DrawPopupShadowBorder(int x, int y, int w, int h);
	void ShadowEdgeH(int x, int y, int count, const uint8_t *src);
	void ShadowEdgeV(int x, int y, int height, const uint8_t *src);
	void OverlayBlock(int x, int y, const uint8_t *src);
	// ---- helpers ----------------------------------------------------------------
	// draw_ui_panel_image_at_xy / _block into a panel buffer of pitch `pitch`.
	void PanelImage(UiCanvas &buf, uint32_t pitch, uint32_t id, int x, int y);
	// A picture blitted into the stack work buffer (pitch 0x64), then `count`
	// bytes of each of `rows` rows from column `from` copied to (x, y) of `buf`.
	void WorkRows(uint32_t id, UiCanvas &buf, int x, int y, uint32_t from, uint32_t count, uint32_t rows);
	void ClearCells(UiCanvas &buf, int x, int y, int w, int h);   // clear_buffer_rows
	// The screen: both pictures.
	void ScreenPut(int x, int y, uint8_t v);
	void ScreenShade(int x, int y, int w, int rows, uint8_t row);
	void Problem(const std::string &s);

	HudAssets mA;
	// The health panel (0x83d6c, 0x4b0 bytes, pitch d[0]) and the weapon panel
	// (0x83d78, 0x1290 bytes, pitch d[0]), and whether each is allocated.
	UiCanvas mHealthBuf, mWeaponBuf;
	bool mHealthLive = false, mWeaponLive = false;
	UiCanvas mWork;                          // the routines' stack work buffer, uint32_t[250], pitch 0x64
	const uint32_t *mDesc = nullptr;         // [0x83e78]: the weapon panel's descriptor
	int32_t mCellCounter = 0;                // 0x83d7c
	int mWeaponX = 0;                        // 0x83e1c: the weapon panel's screen column
	int mHealthX = 0;                        // 0x83e4c: the health panel's screen column
	// The held icon (0x81310 spans, 0x819b0 width, 0x819b4 rows, 0x8130c id).
	std::vector<uint8_t> mSpans;
	int32_t mHeldW = 0, mHeldRows = 0, mHeldId = 0;
	// This frame's pictures, and the ones shown.
	HudCanvas mTop, mHeld, mTopOut, mHeldOut;
	std::vector<std::string> mProblems;
};

} // namespace roth
