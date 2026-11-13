#pragma once
//
// Realms of the Haunting's dialogue text on screen -- the choice menu, the
// subtitle line, the timed message -- drawn as the original draws it: the
// laid-out records (roth_dialogue's, byte for byte the original's) written
// with the original's font by the original's renderers, into an 8-bit picture
// of the rectangle they cover. Engine-free: the C++ standard library and
// roth_uidraw / roth_dialogue, nothing else.
//
// WHERE IT COMES FROM (ROTH.C, roth_c/src/engine); the numbered checklist with
// file:line on both sides is in roth_dlgdraw.cpp:
//   render_dialogue_text_panel        dialogue_ui.c:814-848     the choice menu
//   dbase100_open_dialogue_window     dialogue_ui.c:1189-1212   (its panel rectangle)
//   render_text_ui (the drawing)      dialogue_ui.c:1016-1047   the subtitle line
//   render_active_timed_message       dialogue_ui.c:900-964     the timed message
//   update_dialogue_choice_highlight  dialogue_ui.c:255-266     (its hit areas)
//
// WHAT IS NOT IN THE PICTURE: the original darkens the screen behind each
// line (apply_ui_palette_rect, a remap of whatever the 3D view shows there by
// the reveal ramp). That is the world's pixels, not the text's: the picture
// holds the text and its outline only, and its mask says which pixels those are.
//

#include "roth_uidraw.h"
#include "roth_dialogue.h"

#include <stdint.h>
#include <vector>

namespace roth
{

class DialogueTextPainter : public UiCanvas
{
public:
	// g_screen_pitch (0x85498) and the logical height (0x8549c).
	int screenPitch = 640, screenHeight = 480;
	bool hires = false;

	// render_dialogue_text_panel (dialogue_ui.c:814-848) over the choice
	// records `segs` (g_dialogue_segments, the first `count`), with
	// g_choice_interaction_mode + 0x10 (0x83add) = `originX` (0x28 when no
	// topic menu is up: dialogue_ui.c:1209). The picture covers the panel.
	void RenderDialogueTextPanel(const std::vector<TextLine> &segs, int count, int originX = 0x28);
	// update_dialogue_choice_highlight's hit areas (dialogue_ui.c:255-266) at
	// screen (mx, my): the choice row, -1 none. Valid after RenderDialogueTextPanel.
	int ChoiceRowAt(int mx, int my) const;

	// render_text_ui's drawing of segment `seg` (dialogue_ui.c:1016-1047):
	// under the view (g_view_y + 4 + g_view_h), or under the inventory panel
	// (anchor y + 0x54) while it is open, kept 3 pixels above the screen's foot.
	void RenderTextUiSegment(const TextLine &seg, bool inventoryOpen, int anchorY, int viewY, int viewH);

	// render_active_timed_message's drawing (dialogue_ui.c:907-964): one line
	// at the view's top (g_view_y + 2) or above the open inventory panel; more
	// lines from y 0x20 down, 0xb apart.
	void RenderActiveTimedMessage(const std::vector<TextLine> &lines, int count, bool inventoryOpen, int anchorY, int viewY);

	// The rectangle the original saves and marks dirty for what it drew
	// (screen coordinates, logical rows): the picture covers it with a margin.
	int RegionX() const { return mRx; }
	int RegionY() const { return mRy; }
	int RegionW() const { return mRw; }
	int RegionH() const { return mRh; }

private:
	void Begin(int x, int y, int w, int h);
	int mRx = 0, mRy = 0, mRw = 0, mRh = 0;
	// the choice panel's geometry, for ChoiceRowAt
	std::vector<TextLine> mSegs;
	int mCount = 0, mOriginX = 0x28, mPanelH = 0;
};

} // namespace roth
