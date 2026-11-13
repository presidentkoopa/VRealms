#pragma once
//
// Realms of the Haunting's message boxes -- the main menu, the in-game
// options, the save and load slot lists, the confirmations, the settings and
// movie boxes -- drawn as the original draws them: show_message_box's
// compositor and its loop's drawing (menu_hud_ui.c:1284-1902) into an 8-bit
// picture of the box, from DATA\ICONS.ALL's own frame pictures, with the
// texts of DBASE100/DBASE400 in the original's font, every piece placed by
// the original's routines. The click areas are the original's own
// (hit_test_ui_element). Engine-free: the C++ standard library and
// roth_uidraw / roth_dialogue, nothing else.
//
// WHERE IT COMES FROM (ROTH.C, roth_c/src/engine); the numbered checklist with
// file:line on both sides is in roth_menudraw.cpp:
//   show_message_box             menu_hud_ui.c:1284-1902   phases 1-5, and the loop's drawing
//   render_menu_entry_list       menu_hud_ui.c:872-937
//   draw_menu_value_bar          menu_hud_ui.c:803-844
//   draw_scroll_indicators       menu_hud_ui.c:782-795
//   hit_test_ui_element          menu_hud_ui.c:46-87
//   layout_timed_message_text    dialogue_ui.c:344-480     (roth_dialogue's port)
//   resolve_dbase100_text        dbase100.c:408-423        (through the host)
//   the box descriptors          the EXE image 0x71914-0x71d45 (transcribed; see roth_menudraw.cpp)
//
// WHAT IS NOT IN THE PICTURE: what the original does to the screen AROUND the
// box -- its 4-pixel drop shadow (draw_popup_shadow_border_smc, a blend of
// whatever lies there) -- and every pixel of the box's rectangle that no
// routine writes (the picture's mask says which): they are the world behind.
//

#include "roth_uidraw.h"

#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

namespace roth
{

// A message-box descriptor (show_message_box's ESI, menu_hud_ui.c:1263-1266),
// as the EXE image holds it.
struct MsgBoxEntry
{
	uint32_t text = 0;      // [0] a DBASE100 interface text id (0x31 / 0x32: a numbered save slot)
	uint32_t alt = 0;       // [4] the alternative text id (byte[0xb] & 0x80), or a label table
	uint32_t value = 0;     // [8] the action word; byte[0xb] = its top byte: 0x10 a slider row, 0x80 a toggle's text
};
struct MsgBoxDesc
{
	uint32_t va = 0;        // where it is in the EXE image
	uint32_t title = 0, body = 0;
	uint16_t icon = 0;      // word[8]: an ICONS.ALL slot drawn at (4, 4)
	uint16_t sel = 0;       // word[0xa]: the selection the box opens with (the last one kept)
	uint16_t scroll = 0;    // word[0xc]
	uint16_t range = 0;     // word[0xe]: the slot lists' scroll range
	std::vector<MsgBoxEntry> entries;
};
// The descriptors transcribed from the running original's bytes (0x71914 ..
// 0x71d45; tools/menudraw_test compares them with the original's).
const std::vector<MsgBoxDesc> &MsgBoxDescriptors();
const MsgBoxDesc *FindMsgBoxDesc(uint32_t va);

class MessageBoxPainter : public UiCanvas
{
public:
	// The screen the original composes on: g_screen_pitch (0x85498) and the
	// logical height (0x8549c); 640 x 480 in the retail VideoMode 8.
	int screenPitch = 640, screenHeight = 480;
	bool hires = false;
	uint8_t markerSelected = 0x40;          // g_map_menu_marker_selected 0x7675d
	uint8_t defaultColor5 = 0xee;           // g_default_message_color[5] 0x76763
	// 0x83e8c ("VA_g_active_weapon_ammo_cap + 0x18"): the settings toggles a
	// 0x80 row's text reads. Null: all clear.
	const int32_t *toggles = nullptr;
	// g_ui_panel_anchor_x/y (0x80b24/0x80b28) as they are when the box opens:
	// render_menu_entry_list places the slot rows by them, and a fixed box
	// without flag 0x100 sets them first (compute_viewport_half_extents).
	int anchorX = 0, anchorY = 0;

	// resolve_dbase100_text(dest, maxlen, id, 0) (dbase100.c:408-423): the
	// bytes read, as the original counts them (its "Missing Text" is 13).
	std::function<uint32_t(uint8_t *dest, uint32_t maxlen, uint32_t id)> resolveText;
	// The slot names buffer (read_savegame_slot_names, savegame.c:1262-1294):
	// 9 cells of 0x30 bytes; a cell {0, '~'} is a slot with no file. The save
	// and load rows (text 0x31 / 0x32) read it.
	std::vector<uint8_t> slotNames;
	// read_savegame_thumbnail(slot) (savegame.c:1305-1333): the DAS picture of a
	// save's thumbnail, empty for none. Called with scroll + selection.
	std::function<std::vector<uint8_t>(uint32_t slot)> thumbnail;
	// g_ui_panel_scratch_handle's picture (0x811ac, flag 2: the view as it was
	// when the panel opened). Empty: the handle is 0 and nothing is drawn there.
	std::vector<uint8_t> panelSnapshot;

	// ---- the box ----------------------------------------------------------------------
	// show_message_box's phases 1-5 (menu_hud_ui.c:1292-1548): the box composed
	// as it stands before its loop's first pass. `sel` / `scroll` are the
	// descriptor's words [0xa] / [0xc] at that moment.
	void Open(const MsgBoxDesc &d, uint32_t flags, int sel, int scroll);
	// One pass of the loop's drawing (menu_hud_ui.c:1658-1732) once its input
	// half has decided: `sel` the selection now (-1 / -2 none), `newScroll` the
	// scroll word if this pass moved it (else -1), `ticks` the frame ticks the
	// pass took (g_frame_time_scale; the zoom fade counts them).
	void Pass(int sel, int newScroll, uint32_t ticks);
	bool IsOpen() const { return mOpen; }

	// hit_test_ui_element (menu_hud_ui.c:46-87) with the arguments the loop
	// gives it (:1591-1593), at screen (mx, my): the 1-based row, -1 the close
	// box (or the 0x80 title box), -9 the body box (flag 0x800), -6 / -7 the
	// scroll arrows (flag 8), 0 nothing. The loop then drops -6 at the top and
	// -7 at the bottom (:1595-1596): LoopHit does both.
	int HitTest(int mx, int my) const;
	int LoopHit(int mx, int my) const;

	// The box's geometry on the original's screen, and the rows' count.
	int BoxX() const { return mBoxX; }
	int BoxY() const { return mBoxY; }
	int BoxW() const { return (int)mBoxW; }
	int BoxH() const { return (int)mBoxH; }
	int Rows() const { return (int)mRows; }
	uint32_t Flags() const { return mFlags; }
	const MsgBoxDesc &Desc() const { return mDesc; }
	int Selected() const { return mPrevSel; }
	int Scroll() const { return mScroll; }
	// The shadow ring the original blends around the box (not drawn here):
	// {x, y, w, h} of draw_popup_shadow_border_smc's rectangle; w = 0 none.
	void ShadowRect(int &x, int &y, int &w, int &h) const { x = mShX; y = mShY; w = mShW; h = mShH; }

private:
	void RenderMenuEntryList();
	void DrawMenuValueBar(int x, int y, uint8_t marker, int value);
	void DrawScrollIndicators();
	void Tile(int x, int y, uint32_t slot) { BlitRelocDasImage(x, y, slot); }
	uint32_t Resolve(uint8_t *dest, uint32_t maxlen, uint32_t id);

	bool mOpen = false;
	MsgBoxDesc mDesc;
	uint32_t mFlags = 0;
	int mBoxX = 0, mBoxY = 0;
	uint32_t mBoxW = 0, mBoxH = 0;
	int mRowOff = 0;                        // 0x83e88: 0x2e (fixed box) / 0xd (sized box)
	int mContentY = 0;
	uint32_t mRows = 0;                     // nlines after the title (and body) lines are taken off
	int mHeaderLines = 1;
	int mScroll = 0;
	int mPrevSel = -2;
	int mSelForPass = 0;                    // the descriptor's selection the loop starts with
	bool mThumbRedraw = false;
	// The laid-out lines (layout_timed_message_text's table): [0] the title,
	// [1] the body if any, then one per row. x0 / x1 are what the hit test reads.
	struct Line { int32_t x0 = 0, x1 = 0; std::vector<uint8_t> text; };
	std::vector<Line> mLines;
	Line &Row(uint32_t i) { return mLines[(size_t)mHeaderLines + i]; }
	const Line &Row(uint32_t i) const { return mLines[(size_t)mHeaderLines + i]; }
	// the zoom fade (flag 0x200): the box's block and the ticks counted
	Saved mZoomSave;
	bool mZoom = false;
	uint32_t mTickAccum = 2;
	int mShX = 0, mShY = 0, mShW = 0, mShH = 0;
};

} // namespace roth
