#pragma once
//
// The player-facing half of Realms' dialogue and inventory: one panel system,
// in the world, for the dialogue window, the choice menu, the inventory screen
// and the examine popup. It works flat (keys, and the crosshair as the
// pointer) and in VR (the hand beam is the pointer, the use button clicks).
//
// The RULES are elsewhere and engine-free: roth_dialogue.h (what is shown, for
// how long, what a choice does), roth_invscreen.h (what a click or key on the
// inventory means), roth_inventory.h (the verbs). Everything here decides only
// WHERE things are drawn and turns a beam hit into the row or cell the rules
// take. Layout may differ from the original; the content and the choices do
// not: the text, its colour byte, its line breaks and pages are the
// original's, and the icons are the player's own DBASE200 art.
//
// Engine side. Hooks are narrow and are listed at each declaration.
//

#include <stdint.h>
#include "roth_panelmap.h"

class FLevelLocals;
class AActor;
class player_t;

namespace roth
{

class GameCore;
class Dbase100;
class Install;
class Dbase100Sink;
class InventorySink;
struct GiveContext;
struct Colour;
class TextureSet;

// Once, when the runtime attaches the game core (roth_runtime.cpp BeginLevel).
// Loads DBASE400 (text), DBASE200 (icons) and opens DBASE500 (speech headers)
// from the player's install. Nothing is written anywhere.
void InterfaceAttach(GameCore &core, Dbase100 &db, const Install &install);
// The inventory's sink, for GameCore::Attach.
InventorySink *InterfaceInventorySink();
// The game core's inventory, null before InterfaceAttach (the weapon reads and
// writes slot quantities through it).
class Inventory;
Inventory *InterfaceInventory();

// The calls of the interpreter that belong to the dialogue window. CoreHost
// forwards these here (they replace the console stopgap).
uint32_t InterfaceReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t voiceOffset, uint32_t flag);
uint32_t InterfaceOpenDialogueWindow(const uint8_t *text, uint32_t bytes);
uint32_t InterfaceOpenDialogueWindowAlt(const uint8_t *text, uint32_t bytes);
void InterfaceDialogueVoiceForceEnd(uint32_t eax, uint32_t edx);
int32_t InterfaceInventoryPanelOpen();
int32_t InterfaceInspectPopupActive();
// THE HELD OBJECTS (roth_held.cpp). The item whose examine popup is up and running, 0 none. Read only.
int InterfacePopupItem();
// A HELD ITEM BROUGHT TO THE EYES (ledger row 37): open the inventory as the 'I' key does and, the next tic, the
// original's primary click on its right-hand box -- the examine of the right-hand item (`item`, for the log): its
// popup and, on the first look, its OnInspect; when that popup ends, the original's close key. False (nothing done)
// while a panel, a menu or a line owns the pointer, or one is already under way.
bool InterfaceExamineRightHand(int item);
// The popup's billboard placed in the hands (the Book, owner Q8): at `at`, squared to the eye, its size times
// `scale`; `on` false puts it back where the original's screen has it. Presentation: the same picture, content,
// timing and hit test (the hands' laser reads the billboard where it stands).
void InterfacePopupPlace(bool on, const PanelV3 &at, double scale);
// An item's inventory icon, DBASE200 (load_item_icon_resource, roth_uidraw.h ItemIconPicture). False: none.
bool InterfaceItemIcon(int item, const uint8_t **img, size_t *size);
void InterfaceArmPanelExit();
void InterfaceRequestPanelClose();

// The HUD (roth_huddraw): the weapon panel's redraws where the original's
// weapon logic calls them -- render_weapon_view(arg) (WC:812, 820, 979),
// redraw_weapon_hud_panel (WC:874), render_weapon_hud (WC:935, 1429).
void InterfaceHudWeaponView(int arg);
void InterfaceHudWeaponPanel();
void InterfaceHudRenderWeaponHud();

// The map's pack palette: what the text colour bytes and the icons index.
// From the loader, right after it opens the pack (rothmap.cpp).
void InterfaceSetPalette(const TextureSet &art);
// That palette as the original holds it (g_palette_rgb_ptr, 6-bit VGA values):
// what a movie subtitle's colour byte indexes (dialogue_ui.c:1927-1929).
// False before a map has set one.
bool InterfaceGamePalette6(uint8_t out[0x300]);

// Once per tic from TickLevelLogic, after the chains have run.
void InterfaceTick(FLevelLocals *level);
// The level is going away: its billboards go with it.
void InterfaceEndLevel();

// A use press (P_UseLines, before anything else looks at it). True when the
// press belonged to a panel or to the dialogue and must go no further.
bool InterfaceUse(player_t *player);

// True while a panel owns the pointer -- a menu box, the inspect popup, the
// inventory screen, or the dialogue waiting on a click or a choice -- so the
// VR hands' world picker (roth_vrpick.cpp) stands down and leaves the beam to
// the panel, as the original's cursor belongs to whichever of them is up.
bool InterfacePointerBusy();
// Which of them, for a log line: "menu", "popup", "inventory", "dialogue", or null.
const char *InterfacePointerOwner();

// THE VR HANDS ON A PANEL (roth_vrpick.cpp, roth_invhand.h). Each hand is the
// original's mouse on the panel that owns the pointer: the inventory screen,
// or the examine popup over it (a menu box and the dialogue are not worked by
// the hands). The panel as a plane carrying the original's pixels
// (roth_panelmap.h), its front on the side of `eye`: HANDPANEL_NONE when no
// such panel is up.
enum { HANDPANEL_NONE = 0, HANDPANEL_INVENTORY = 1, HANDPANEL_POPUP = 2 };
int InterfaceHandPanel(const PanelV3 &eye, PanelPlane &out);
// The hand's mouse at pixel (x, y) of that panel (`onPicture`: on the picture
// itself): the hover, as the original's handle_cursor_click with no button
// (roth_invmouse.h; the popup: its pointer). Returns the cursor shape the
// original would show on the inventory (no cursor is drawn), else 0.
uint16_t InterfaceHandPointer(int panel, int x, int y, bool onPicture);
// A click of `button` (1 primary = the left button, 2 secondary = the right)
// there: exactly what a click of the crosshair or the use key there does
// (PanelClick / the popup's Press). Returns the inventory action it hit
// (dispatch_dialogue_ui_action's), -1 for the popup.
int InterfaceHandClick(int panel, int x, int y, bool onPicture, int button);
// Whether a hand drove the pointer this tic: the crosshair / aim-pose hover
// stands down on the next one so the cursor follows the finger alone.
void InterfaceHandsDrivePointer(bool on);
// THE DIALOGUE OWNS THE POINTER (a choice menu up, or a line waiting for a click:
// InterfacePointerOwner "dialogue"): a VR hand's button press is a click of the
// original's there (game_core.c:604-632) -- it skips the line, or picks the choice
// row the hand's ray is on. False when the dialogue does not own the pointer.
bool InterfaceHandDialogueClick(const PanelV3 &start, const PanelV3 &dir);

// The Realms menus (roth_menus.h), drawn as a world panel like the others and
// clicked with the same beam or crosshair. While a box is up it is the only
// panel that takes clicks; the inventory screen gains an "Options" button that
// opens the in-game options, as the original's inventory screen does
// (inventory.c:1799-1817). Set once by the runtime.
class RealmsMenus;
void InterfaceSetMenus(RealmsMenus *menus);

// An object the player used or examined that carries no command chain: the
// original's world pickup (activate_targeted_object, input.c:1034-1086) and
// right-click examine (examine_object_under_cursor, input.c:982-1014).
// `use` false is the examine. True when it acted. Destroys `mo` on a pickup.
bool InterfaceObjectAction(AActor *mo, int rothSector, int rothIndex, bool use);

} // namespace roth
