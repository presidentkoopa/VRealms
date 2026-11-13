//
// Realms' menus, without drawing. See roth_menus.h.
//
// THE BOXES, as the original's EXE image holds them (show_message_box's
// descriptor: [0] title text id, [4] body text id, [0x10] entry count,
// entries of { text id, alt, value } from +0x14; menu_hud_ui.c:1263-1278).
// Read out of the running original's memory at boot and decoded with the
// retail DBASE100/DBASE400 (tools/menus_test prints the same table from the
// constants below, so a reader can compare):
//
//   0x71bbd main      "Options" / "Options menu": Play the game (75), Load a
//                     game (14), Replay Intro (76, -> settings item 3),
//                     Settings (47, -> item 1), Quit to DOS (43, -> item 0)
//   0x71b6d options   same title: New game (9, -> item 2), Load a game (14),
//                     Save a game (15), Settings (47), Quit to DOS (43)
//   0x71c0d save      "Save a game" / "Select file to save": 5 rows, empty =
//                     "Empty slot" (50)
//   0x71c5d load      "Load a game" / "Select file to load": 5 rows, empty =
//                     "Not available" (49)
//   0x71a00 quit      "Quit to DOS": Quit to DOS (43), CANCEL (17)
//   0x71a2c new game  "New game": OK to start a new game. (53), CANCEL (17)
//   0x71cad replace   "Save a game": OK to replace (39), CANCEL (17)
//   0x71914 clips     "Movie clips": Intro (78, action 5), Making of (79,
//                     action 6), Credits (80, action 7)
//   0x71a9c settings  "Settings" (front end): Volume settings (18), Subtitle
//                     settings (19), Input Settings (48)
//   0x71a58 settings  (in game): the same with Screen settings (44) third
//

#include "roth_menus.h"

namespace roth
{

namespace
{

struct Desc
{
	uint32_t title, body;
	std::vector<uint32_t> rows;
};

const Desc &BoxDesc(RealmsMenus::Box b, bool frontEnd)
{
	static const Desc main     = { 10, 62, { 75, 14, 76, 47, 43 } };
	static const Desc options  = { 10, 62, { 9, 14, 15, 47, 43 } };
	static const Desc load     = { 14, 37, {} };
	static const Desc save     = { 15, 38, {} };
	static const Desc quit     = { 43, 0,  { 43, 17 } };
	static const Desc newgame  = { 9,  0,  { 53, 17 } };
	static const Desc replace  = { 15, 0,  { 39, 17 } };
	static const Desc movies   = { 72, 0,  { 78, 79, 80 } };
	static const Desc setFront = { 47, 0,  { 18, 19, 48 } };
	static const Desc setGame  = { 47, 0,  { 18, 19, 44, 48 } };
	static const Desc none     = { 0, 0, {} };
	switch (b)
	{
	case RealmsMenus::MAIN: return main;
	case RealmsMenus::OPTIONS: return options;
	case RealmsMenus::LOAD: return load;
	case RealmsMenus::SAVE: return save;
	case RealmsMenus::QUIT_CONFIRM: return quit;
	case RealmsMenus::NEWGAME_CONFIRM: return newgame;
	case RealmsMenus::OVERWRITE_CONFIRM: return replace;
	case RealmsMenus::MOVIES: return movies;
	case RealmsMenus::SETTINGS: return frontEnd ? setFront : setGame;    // menu_hud_ui.c:1942-1943
	default: return none;
	}
}

} // namespace

// A box shown afresh: its selection and scroll from its descriptor's words
// (menu_hud_ui.c:1545, 1289), the pointer's state reset (:1533-1536).
void RealmsMenus::Show()
{
	if (!mWordsSet)
	{
		// The EXE image's word[0xa] at boot (tools/menudraw_test reads it): 0x71a00
		// (quit?) and 0x71a2c (new game?) open on their second row, CANCEL.
		for (int &w : mSelWord) w = 0;
		for (int &w : mScrollWord) w = 0;
		mSelWord[QUIT_CONFIRM] = 1;
		mSelWord[NEWGAME_CONFIRM] = 1;
		mWordsSet = true;
	}
	const Box b = Current();
	mSelected = mSelWord[b];
	mScroll = (b == LOAD || b == SAVE) ? mScrollWord[b] : 0;
	mKbNav = true;
	mPrevHit = -2;
	mShowSerial++;
	switch (b)
	{
	case MAIN: mShowFlags = mMainShown ? 1u : 0x201u; mMainShown = true; break;
	case OPTIONS: mShowFlags = 0x8c1; break;
	case LOAD: mShowFlags = mFrontEnd ? 0xdu : 0x4du; break;
	case SAVE: mShowFlags = 0x4f; break;
	case OVERWRITE_CONFIRM: mShowFlags = 0x200; break;
	default: mShowFlags = 0; break;
	}
}

// show_message_box's teardown keeps the selection when there is one
// (menu_hud_ui.c:1887-1888); the slot lists hand their scroll to each other
// (run_options_menu / run_main_menu, menu_hud_ui.c:2050, 2060, 2096).
void RealmsMenus::Keep()
{
	const Box b = Current();
	if (b == NONE) return;
	if (mSelected >= 0) mSelWord[b] = mSelected;
	if (b == LOAD || b == SAVE) { mScrollWord[LOAD] = mScroll; mScrollWord[SAVE] = mScroll; }
}

void RealmsMenus::Push(Box b)
{
	Keep();
	mStack.push_back(b);
	Show();
}

void RealmsMenus::Pop()
{
	Keep();
	if (!mStack.empty()) mStack.pop_back();
	if (mStack.empty()) { mFrontEnd = false; mSelected = 0; return; }
	Show();                                 // the box under it is shown again (its caller's loop)
}

void RealmsMenus::CloseAll()
{
	Keep();
	mStack.clear();
	mSelected = 0;
	mScroll = 0;
	mFrontEnd = false;
}

void RealmsMenus::Emit(MenuOutcome::Kind k, int slot, const std::string &name)
{
	mOut.kind = k;
	mOut.slot = slot;
	mOut.name = name;
	CloseAll();
}

void RealmsMenus::OpenMain()
{
	CloseAll();
	mFrontEnd = true;                       // g_menu_frontend_flag = 1, menu_hud_ui.c:2086
	mMainShown = false;                     // the first show: flags 0x201 (:2082)
	Push(MAIN);
}

void RealmsMenus::OpenOptions()
{
	CloseAll();
	mFrontEnd = false;                      // g_menu_frontend_flag = 0, menu_hud_ui.c:2041
	Push(OPTIONS);
}

void RealmsMenus::OpenDeath(bool canLoad)
{
	CloseAll();
	mDeathCanLoad = canLoad;
	mTitleText = "You are dead";
	Push(DEATH);
	mSelected = canLoad ? 0 : 1;
}

uint32_t RealmsMenus::TitleId() const { return BoxDesc(Current(), mFrontEnd).title; }
uint32_t RealmsMenus::BodyId() const { return BoxDesc(Current(), mFrontEnd).body; }

std::vector<RealmsMenus::Row> RealmsMenus::Rows() const
{
	std::vector<Row> out;
	const Box b = Current();
	if (b == LOAD || b == SAVE)
	{
		for (int r = 0; r < VISIBLE_SLOTS; r++)
		{
			const int slot = SlotOfRow(r);
			Row row;
			const std::string &n = mSlotNames[(size_t)(slot - 1)];
			if (!n.empty()) row.label = n;
			else row.textId = b == LOAD ? 49 : 50;          // "Not available" / "Empty slot"
			row.enabled = b == SAVE || !n.empty();
			out.push_back(row);
		}
		return out;
	}
	if (b == DEATH)
	{
		Row load; load.label = "Load the last saved game"; load.enabled = mDeathCanLoad;
		Row cont; cont.label = "Continue";
		out.push_back(load);
		out.push_back(cont);
		return out;
	}
	for (uint32_t id : BoxDesc(b, mFrontEnd).rows)
	{
		Row row;
		row.textId = id;
		// Settings submenus are listed and not ported: volume, subtitles,
		// screen and input belong to the engine's own options here.
		if (b == SETTINGS) row.enabled = false;
		out.push_back(row);
	}
	return out;
}

bool RealmsMenus::CanScrollUp() const { return (Current() == LOAD || Current() == SAVE) && mScroll > 0; }
bool RealmsMenus::CanScrollDown() const { return (Current() == LOAD || Current() == SAVE) && mScroll + VISIBLE_SLOTS < SLOTS; }
void RealmsMenus::ScrollUp() { if (CanScrollUp()) mScroll--; }
void RealmsMenus::ScrollDown() { if (CanScrollDown()) mScroll++; }

// The arrow keys (menu_hud_ui.c:1641-1657): at the first row of a list
// with a range Up scrolls, else the selection moves and wraps; Down alike.
void RealmsMenus::Up()
{
	mKbNav = true;
	const bool list = Current() == LOAD || Current() == SAVE;
	if (mSelected == 0 && list) { ScrollUp(); return; }
	mSelected--;
	if (mSelected < 0) mSelected = (int)Rows().size() - 1;
}

void RealmsMenus::Down()
{
	mKbNav = true;
	const int n = (int)Rows().size();
	const bool list = Current() == LOAD || Current() == SAVE;
	if (list && mSelected == n - 1) { ScrollDown(); return; }
	mSelected++;
	if ((unsigned)mSelected >= (unsigned)n) mSelected = 0;
}

void RealmsMenus::PointerAt(int hit)
{
	if (hit == -6 && !CanScrollUp()) hit = 0;                // :1595-1596
	if (hit == -7 && !CanScrollDown()) hit = 0;
	if (hit == 0) { if (!mKbNav) mSelected = -2; }           // :1597-1598
	else if (hit != mPrevHit)                                // :1600-1606
	{
		mSelected = hit != -1 ? hit - 1 : -1;
		mKbNav = false;
	}
	mPrevHit = hit;
}

void RealmsMenus::PointerClick(int hit)
{
	PointerAt(hit);
	if (hit == -6 && CanScrollUp()) { ScrollUp(); return; }      // key 0xfe
	if (hit == -7 && CanScrollDown()) { ScrollDown(); return; }  // key 0xfd
	if (hit == -9) return;                                       // key 0x32: the gallery, not ported
	if (hit == 0) return;
	if (mSelected >= 0) Choose(mSelected);                       // key 0x1c
	else if (mSelected > -2) Back();                             // key 1
}

bool RealmsMenus::ParentVisible() const
{
	const Box b = Current();
	return mStack.size() >= 2 && (b == QUIT_CONFIRM || b == NEWGAME_CONFIRM || b == MOVIES || b == SETTINGS);
}

int RealmsMenus::ParentSelected() const
{
	return mStack.size() >= 2 ? mSelWord[mStack[mStack.size() - 2]] : 0;
}

uint32_t RealmsMenus::DescriptorOf(Box b, bool frontEnd)
{
	switch (b)
	{
	case MAIN: return 0x71bbd;
	case OPTIONS: return 0x71b6d;
	case LOAD: return 0x71c5d;
	case SAVE: return 0x71c0d;
	case QUIT_CONFIRM: return 0x71a00;
	case NEWGAME_CONFIRM: return 0x71a2c;
	case OVERWRITE_CONFIRM: return 0x71cad;
	case MOVIES: return 0x71914;
	case SETTINGS: return frontEnd ? 0x71a9c : 0x71a58;
	default: return 0;
	}
}

void RealmsMenus::Choose(int row)
{
	const std::vector<Row> rows = Rows();
	if (row < 0 || row >= (int)rows.size() || !rows[(size_t)row].enabled) return;
	mSelected = row;
	switch (Current())
	{
	case MAIN:                                              // run_main_menu, menu_hud_ui.c:2083-2107
		if (row == 0) Emit(MenuOutcome::PLAY);              // m <= 1 -> 0xa
		else if (row == 1) Push(LOAD);                      // m == 2
		else if (row == 2) Push(MOVIES);                    // value 0x02000003 -> run_settings_menu(3)
		else if (row == 3) Push(SETTINGS);                  // 0x02000001 -> (1)
		else Push(QUIT_CONFIRM);                            // 0x02000000 -> (0)
		break;
	case OPTIONS:                                           // run_options_menu, menu_hud_ui.c:2036-2073
		if (row == 0) Push(NEWGAME_CONFIRM);                // 0x02000002 -> (2)
		else if (row == 1) Push(LOAD);
		else if (row == 2) Push(SAVE);
		else if (row == 3) Push(SETTINGS);
		else Push(QUIT_CONFIRM);
		break;
	case LOAD:
		Emit(MenuOutcome::LOAD, SlotOfRow(row));            // (slot << 16) + 3
		break;
	case SAVE:
	{
		const int slot = SlotOfRow(row);
		// write_savegame_file: an existing slot asks first (savegame.c:1539-1548).
		if (!mSlotNames[(size_t)(slot - 1)].empty()) { mPendingSaveSlot = slot; Push(OVERWRITE_CONFIRM); }
		else Emit(MenuOutcome::SAVE, slot, mDefaultName);
		break;
	}
	case OVERWRITE_CONFIRM:
		// prompt_save_overwrite == 1 writes; anything else returns 0 and the
		// options menu has already closed (inventory.c:1810).
		if (row == 0) Emit(MenuOutcome::SAVE, mPendingSaveSlot, mDefaultName);
		else Emit(MenuOutcome::RESUME);
		break;
	case QUIT_CONFIRM:                                      // run_settings_menu case 0, menu_hud_ui.c:1935-1938
		if (row == 0) Emit(MenuOutcome::QUIT);
		else Pop();
		break;
	case NEWGAME_CONFIRM:                                   // case 2, :2016-2019
		if (row == 0) Emit(MenuOutcome::NEW_GAME);
		else Pop();
		break;
	case MOVIES:                                            // case 3: the 0x400000 rows run a record and the box stays
		mOut.kind = MenuOutcome::RUN_ACTION;
		mOut.slot = 5 + row;                                // values 0x00400005..7
		break;
	case DEATH:
		Emit(row == 0 ? MenuOutcome::DEATH_LOAD : MenuOutcome::DEATH_END);
		break;
	default:
		break;
	}
}

void RealmsMenus::Back()
{
	switch (Current())
	{
	case MAIN: Emit(MenuOutcome::PLAY); break;              // Esc: raw 0 -> m <= 1 -> play (menu_hud_ui.c:2092-2093)
	case OPTIONS: Emit(MenuOutcome::RESUME); break;         // result 0 (inventory.c:1803)
	case OVERWRITE_CONFIRM: Emit(MenuOutcome::RESUME); break;
	case DEATH: Emit(MenuOutcome::DEATH_END); break;        // Esc (game_core.c:982-983)
	case NONE: break;
	default: Pop(); break;                                  // a sub-box's cancel returns to its parent
	}
}

std::string RealmsMenus::Describe() const
{
	static const char *names[] = { "none", "main", "options", "load", "save", "quit?", "new game?", "replace?", "movie clips", "settings", "death" };
	std::string s = names[Current()];
	s += " sel " + std::to_string(mSelected);
	if (Current() == LOAD || Current() == SAVE) s += " scroll " + std::to_string(mScroll);
	return s;
}

} // namespace roth
