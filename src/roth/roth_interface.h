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

class FLevelLocals;
class AActor;
struct player_t;

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
void InterfaceArmPanelExit();
void InterfaceRequestPanelClose();

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
