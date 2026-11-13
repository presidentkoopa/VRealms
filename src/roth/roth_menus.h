#pragma once
//
// Realms' menus: the front-end main menu, the in-game options, the save and
// load slot lists, the confirmations, the movie clips, and the death screen --
// as the original has them, without any drawing.
//
// What a box shows and what each row does is ROTH.C's: run_main_menu,
// run_options_menu and run_settings_menu (menu_hud_ui.c:1931-2107) over the
// message-box descriptors in the EXE image (show_message_box's layout,
// menu_hud_ui.c:1263-1278). The descriptors were read out of the running
// original (its game memory at boot, 0x71914-0x71cad) and are transcribed in
// roth_menus.cpp with their addresses; every row's text is a DBASE100
// interface text id, resolved the original's way (resolve_dbase100_text,
// dbase100.c:408-422) by the presentation.
//
// How the rows are drawn and pointed at -- world panels, the VR hand beam,
// the flat crosshair -- is roth_interface's. This module only says which box
// is up, which row is selected, and what the host must do when a row is
// chosen. Standard library only; tools/menus_test runs it.
//

#include <stdint.h>
#include <string>
#include <vector>

namespace roth
{

// What the host must do after a choice. One at a time; Take() hands it over.
struct MenuOutcome
{
	enum Kind : uint8_t
	{
		NONE,
		PLAY,           // main menu "Play the game" (or Esc there): the new-game block, game_core.c:821-845
		LOAD,           // load save slot `slot` (1..9; 0 = the quick slot)
		SAVE,           // save into slot `slot`, named `name`
		NEW_GAME,       // options "New game", confirmed: reset_and_start_new_game, then the new-game block
		QUIT,           // "Quit to DOS", confirmed: action 2, then the program ends (game_core.c:799-805)
		RUN_ACTION,     // a movie clip: DBASE100 action `slot` (show_message_box's 0x400000 rows)
		RESUME,         // the in-game options closed: back to the game
		DEATH_LOAD,     // the death wait's 0x39 key: load the last slot (game_core.c:984-997)
		DEATH_END,      // the death wait's Esc: end the wait now (game_core.c:982-983)
	};
	Kind kind = NONE;
	int slot = 0;
	std::string name;
};

class RealmsMenus
{
public:
	enum Box : uint8_t
	{
		NONE,
		MAIN,               // 0x71bbd
		OPTIONS,            // 0x71b6d
		LOAD,               // 0x71c5d
		SAVE,               // 0x71c0d
		QUIT_CONFIRM,       // 0x71a00
		NEWGAME_CONFIRM,    // 0x71a2c
		OVERWRITE_CONFIRM,  // 0x71cad
		MOVIES,             // 0x71914, "Replay Intro"
		SETTINGS,           // 0x71a9c
		DEATH,              // not a box in the original: its death wait, shown
	};

	static const int SLOTS = 9;             // read_savegame_slot_names, savegame.c:1266
	static const int VISIBLE_SLOTS = 5;     // the slot boxes' five rows (menu_hud_ui.c:2042-2044)

	struct Row
	{
		uint32_t textId = 0;        // a DBASE100 interface text id; 0 = use `label`
		std::string label;          // a slot's saved name, or the death screen's own words
		bool enabled = true;
		bool slider = false;        // a settings slider row (shown, not ported)
	};

	// What the save folder holds: one name per slot 1..9, empty = no save.
	// The host fills this before opening a slot list (read_savegame_slot_names).
	void SetSlotNames(const std::vector<std::string> &names) { mSlotNames = names; mSlotNames.resize(SLOTS); }
	// The name a new save gets (the original types one in a text row; this
	// port names it for the player, VR has no keyboard).
	void SetDefaultSaveName(const std::string &n) { mDefaultName = n; }

	// ---- opening ---------------------------------------------------------
	void OpenMain();                        // run_main_menu, game_core.c:806-816
	void OpenOptions();                     // run_options_menu, from the inventory screen (inventory.c:1799-1817)
	// The death wait (game_core.c:978-1003) as a box with its two keys as rows.
	// `canLoad`: a slot has been saved or loaded this session (g@0x7138c != -1).
	void OpenDeath(bool canLoad);
	bool DeathCanLoad() const { return mDeathCanLoad; }   // a last slot to load (game_core.c:989)
	// The death wait's two keys (game_core.c:986-1000): Space (scancode 0x39)
	// loads the last slot, or with none ends the wait; Esc (scancode 1) ends it.
	void DeathKey(bool space);
	void CloseAll();

	Box Current() const { return mStack.empty() ? NONE : mStack.back(); }
	bool IsOpen() const { return !mStack.empty(); }
	bool FrontEnd() const { return mFrontEnd; }

	// ---- what is shown -----------------------------------------------------
	uint32_t TitleId() const;               // desc[0]
	uint32_t BodyId() const;                // desc[1]
	const std::string &TitleText() const { return mTitleText; }   // DEATH only
	std::vector<Row> Rows() const;          // the visible rows, top to bottom
	int Selected() const { return mSelected; }     // -1 / -2: none (the pointer is off the rows)
	// Slot lists: the first slot shown (the scroll offset, word[0xc]).
	int Scroll() const { return mScroll; }
	bool CanScrollUp() const;
	bool CanScrollDown() const;

	// ---- input -------------------------------------------------------------
	// A pointer click on visible row `row` (0-based), or Enter on the selection.
	void Choose(int row);
	// Enter (key 0x1c): the selection, or with none "cancelled" (menu_hud_ui.c:1743-1744).
	void Enter() { if (mSelected < 0) Back(); else Choose(mSelected); }
	// Esc: a box's "cancelled" (result 0), which each caller reads its own way.
	void Back();
	void Up();
	void Down();
	void ScrollUp();
	void ScrollDown();
	void Hover(int row) { if (row >= 0 && row < (int)Rows().size()) mSelected = row; }
	// The pointer, as show_message_box's loop reads it (menu_hud_ui.c:1591-1607):
	// `hit` is hit_test_ui_element's answer where the pointer is (MessageBoxPainter::
	// LoopHit): a 1-based row, -1 the close box, -6 / -7 the arrows, -9 the
	// picture, 0 nothing. On a new hit the selection follows it (hit - 1);
	// on nothing, after the pointer took over, there is no selection (-2).
	void PointerAt(int hit);
	// A click with the pointer at `hit` (:1626-1638): an arrow scrolls, a row
	// is chosen, the close box cancels.
	void PointerClick(int hit);
	// show_message_box's flags for the box on top, as its caller passes them
	// (menu_hud_ui.c:2041, 2049, 2059, 2082-2095; run_settings_menu; savegame.c's
	// prompt_save_overwrite: 0x200); 0 for DEATH, which is no box.
	uint32_t ShowFlags() const { return mShowFlags; }
	// Bumped every time a box is shown afresh (opened, or shown again after a
	// box over it closed): the presentation composes its picture again.
	uint32_t ShowSerial() const { return mShowSerial; }
	// Whether the box under the top one stays on screen: the boxes
	// run_settings_menu opens from inside their parent's loop (the confirmations,
	// settings, movie clips) are drawn over it; the slot lists replace it.
	bool ParentVisible() const;
	Box Parent() const { return mStack.size() >= 2 ? mStack[mStack.size() - 2] : NONE; }
	int ParentSelected() const;
	// The descriptor show_message_box gets for a box (0 for DEATH).
	static uint32_t DescriptorOf(Box b, bool frontEnd);
	const std::vector<std::string> &SlotNames() const { return mSlotNames; }

	// The pending outcome, once.
	MenuOutcome Take() { MenuOutcome o = mOut; mOut = MenuOutcome(); return o; }
	bool HasOutcome() const { return mOut.kind != MenuOutcome::NONE; }

	// A short line naming the state, for logs and tests.
	std::string Describe() const;

private:
	void Push(Box b);
	void Pop();
	void Emit(MenuOutcome::Kind k, int slot = 0, const std::string &name = std::string());
	int SlotOfRow(int row) const { return mScroll + row + 1; }

	void Show();
	void Keep();                            // the top box's selection into its word[0xa], as its teardown does
	std::vector<Box> mStack;
	bool mFrontEnd = false;
	bool mMainShown = false;                // run_main_menu's first show is 0x201, then 1
	uint32_t mShowSerial = 0;
	uint32_t mShowFlags = 0;
	// Each descriptor's word[0xa] (the selection it opens with) and the slot
	// lists' word[0xc] (their scroll), kept across shows as the EXE image keeps
	// them; the starting values are the image's (quit? and new game? open on CANCEL).
	int mSelWord[DEATH + 2] = {};
	int mScrollWord[DEATH + 2] = {};
	bool mWordsSet = false;
	bool mKbNav = true;                     // [ebp+0x1e]
	int mPrevHit = -2;                      // [ebp+0x12]
	int mSelected = 0;
	int mScroll = 0;
	int mPendingSaveSlot = 0;
	bool mDeathCanLoad = false;
	std::string mTitleText;
	std::vector<std::string> mSlotNames = std::vector<std::string>(SLOTS);
	std::string mDefaultName;
	MenuOutcome mOut;
};

} // namespace roth
