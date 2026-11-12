#pragma once
//
// Realms of the Haunting's inventory: the slot array and the rules that touch
// it. Engine-free, like the readers: the C++ standard library and the
// DBASE100 reader, nothing else.
//
// WHERE THE RULES COME FROM. Every rule here was read out of ROTH.C, the
// verified C reconstruction of the original engine -- roth_c/src/engine/
// inventory.c first, and the helpers its header says live elsewhere
// (renderer.c, weapon_combat.c, dialogue_ui.c, dbase100.c, file_config.c,
// savegame.c). Each rule cites `file.c:LINE`. The first version of this file
// was written from doc summaries; the audit against the source is recorded
// rule by rule in roth_inventory.cpp ("AUDIT:").
//
// [unverified] marks every place where ROTH.C does not give an answer -- almost
// always because the original reads through a null or out-of-range pointer
// there, which a DOS extender tolerates and this cannot reproduce. Nothing is
// guessed without that mark.
//
// THE STATE (g_names.h; inventory.c:12-14)
//   g_inventory_slots  0x80c30  256 x {u16 item_id, u16 quantity}; id 0 = empty;
//                               bit 15 of the id = hidden by the RAW 0x42 filter
//   g_inventory_count  0x80c2c  used-slot count, max 0x100
//   g_selected_item_secondary 0x81038  LEFT hand: the equipped weapon (slot ptr)
//   g_selected_item_primary   0x81044  RIGHT hand: the selected usable item
//   0x8104c / 0x81054           the "Adam" slot and its item id (restore_active_held_item)
//   g_current_cursor_entry 0x7fef0     the entry held on the inventory cursor
//   g_item_autoselected_flag  0x89f60  set when a select-mode query auto-selects
//   g_object_select_easy_flag 0x81e34  user toggle: when set, scripts never auto-select
//   g_last_item_record 0x89fa8  last item given or matched
//   g_item_pickup_flags 0x7fd84 the pickup lock (bit 0 = armed) and its timer at +0x1c
//
// A slot's id is the DBASE100 inventory table index (1-based), NOT the world
// render id. ROTH.C keeps hand SLOT POINTERS, 0 for none; this keeps slot
// indices, -1 for none. The cursor-entry table (0x7fef4) is presentation; where
// ROTH.C goes through an entry to reach a slot, this takes the slot.
//
// 16-BIT SEMANTICS. Slot ids are compared as ROTH.C compares them: sometimes as
// the raw u16, sometimes sign-extended to 32 bits (so a hidden id is NEGATIVE
// and matches no positive id). Each rule says which.
//

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace roth
{

class Dbase100;
struct DbItem;
struct DbTriggerBlock;

// The consequences of an inventory change that belong to OTHER systems
// (weapons, HUD, text, the interpreter, the panel). Each default does nothing;
// the engine side overrides what it has built. Each is named for the ROTH.C
// routine or global write it stands for.
class InventorySink
{
public:
	virtual ~InventorySink() {}

	// --- weapons (weapon_combat.c) ---
	// activate_weapon_item(0, 0) (0x184ab): back to fists. The model has already
	// cleared the left hand (weapon_combat.c:433).
	virtual void UnequipWeapon() {}
	// activate_weapon_item(slot, id) with a real slot. The model has already
	// put the slot in the left hand. NOTE for the implementer: the original
	// goes on to call query_player_inventory(ammo id, 0) (weapon_combat.c:450),
	// which CLEARS g_item_autoselected_flag as a side effect -- call
	// Inventory::Query from here to keep that.
	virtual void EquipWeapon(int slot, int32_t itemId) { (void)slot; (void)itemId; }
	// g_inspect_popup_state+4 (0x810c4) += 1 at inventory.c:427: the "weapon
	// HUD needs a redraw" counter close_inventory_panel tests (inventory.c:1677).
	virtual void WeaponHudDirty() {}
	// rebuild_weapon_inventory_list (0x2245c).
	virtual void RebuildWeaponList() {}
	// reset_weapon_hud (0x1be8e), from the 0x42 filter.
	virtual void ResetWeaponHud() {}
	// `id == [0x811e4]`: is this the active weapon's ammo item? The id is the
	// SIGN-EXTENDED 16-bit value ROTH.C compares against the 32-bit global
	// (inventory.c:263-264, 438-439). AUDIT: was uint16_t.
	virtual bool IsActiveAmmo(int32_t itemId) { (void)itemId; return false; }
	// [0x83e80] = 1 and g_active_weapon_ammo (0x83e6c) += delta
	// (inventory.c:265-266, 440-441).
	virtual void AmmoDisplayChanged(int delta) { (void)delta; }

	// --- the right-hand icon and the panel (presentation) ---
	// update_selected_item_icon (0x1bb4b) after the right hand changed. `slot`
	// is -1 when the hand was emptied.
	virtual void RightHandChanged(int slot) { (void)slot; }
	// g_held_item_slide_timer (0x819bc) = 0 (inventory.c:486, 1411).
	virtual void HeldItemSlideReset() {}
	// clear_dual_array_80afc (0x1c59e): zero the five per-tab cursor positions
	// and the five scroll offsets (renderer.c:10843-10848).
	virtual void ClearTabCursors() {}
	// refresh_inventory_panel (0x1bf7b). The model clears the held cursor
	// itself afterwards, as build_inventory_entry_list does (inventory.c:1317).
	virtual void RefreshPanel() {}
	// g_ui_panel_anchor_y+4 (0x80b2c) != 0: the panel has been asked to close,
	// so use_item_on_self skips its refresh (inventory.c:774).
	virtual bool PanelCloseRequested() { return false; }
	// begin_item_pickup_lock's coordinates (renderer.c:9880-9887): the fly-in
	// starts at (x, y, z) showing world id `renderId`. The model arms the lock.
	virtual void PickupFlyStarted(int32_t x, int32_t y, int32_t z, uint16_t renderId)
	{ (void)x; (void)y; (void)z; (void)renderId; }

	// --- text (dbase100.c) ---
	// A use/combine system message: resolve_dbase100_text(interfaceIndex) is a
	// printf format (0x2a "can't use", 0x2d "used": one %s; 0x29 "can't
	// combine", 0x28 "combined": two), filled with the item names read by
	// read_next_dialogue_line(record+0x10, max 0x62, flag 0), then
	// queue_timed_message_color with colour g_default_message_color[colorIndex]
	// (0x7675e + 0 for a refusal, + 5 for a success). nameB is 0 for the
	// one-name messages. Names are the RAW record+0x10 offsets -- not the
	// ChangeName override (inventory.c:704, 812, 819).
	virtual void ItemMessage(int interfaceIndex, int colorIndex, uint32_t nameA, uint32_t nameB)
	{ (void)interfaceIndex; (void)colorIndex; (void)nameA; (void)nameB; }

	// --- the DBASE100 interpreter (dbase100.c) ---
	// eval_dialogue_record_condition_with_cleanup(block, mode) (0x1db89) on one
	// trigger block of an item: run (mode 0), run with flag 1, or scan (mode
	// 2) the block's commands. Returns the interpreter's result; non-zero is
	// "true" to every caller here.
	virtual int32_t EvalItemBlock(int32_t itemId, const DbTriggerBlock &block, int mode)
	{ (void)itemId; (void)block; (void)mode; return 0; }
	// eval_dialogue_record_by_id(id) then finish_dialogue_record_eval()
	// (0x1dc73, 0x1db5e): run an ACTION record -- the 0x35 "effect" of an OnUse
	// recipe (inventory.c:760-773, 914-927).
	virtual void RunActionRecord(uint32_t actionId) { (void)actionId; }
};

// rng_next_index_for_count (0x1c9a0, renderer.c:11130-11147). Injectable so
// tests can pin the pick; RothIndexRng below is the original generator.
class InventoryRng
{
public:
	virtual ~InventoryRng() {}
	virtual int32_t NextIndexForCount(int32_t count) = 0;
};

// The original generator, transcribed. THE SEED QUESTION, as far as ROTH.C
// answers it: the LCG word at 0x71364 starts as 0xB785 (the EXE's initialised
// data, obj3_owned.c g_inventory_tab_context_map +0x128), the rng word 0x81e38
// and the countdown 0x81e3a start as 0 (uninitialised data, obj3_owned.c:2388),
// and no lifted code other than this routine touches any of the three -- so
// the sequence is fixed from program start, not from a clock. [unverified]:
// that nothing outside the lifted C (startup, savegame restore) writes them.
// ONE instance must be shared with the DBASE100 interpreter, which draws from
// the same generator (dbase100.c:753).
class RothIndexRng : public InventoryRng
{
public:
	int32_t NextIndexForCount(int32_t count) override;

	uint16_t seed = 0xB785;     // 0x71364
	uint16_t rng = 0;           // 0x81e38
	int32_t countdown = 0;      // 0x81e3a
};

struct InventorySlot
{
	uint16_t id = 0;        // DBASE100 inventory id; bit 15 = hidden
	uint16_t quantity = 0;
};

// give_item's context record: the fields it reads from the 16-byte world
// object (or from cmd_give_item's drop-position struct, raw_commands.c:4810-4822).
struct GiveContext
{
	int16_t x = 0;          // +0x00
	int16_t y = 0;          // +0x02
	uint16_t id = 0;        // +0x04  world render id (textureIndex | source << 8)
	uint8_t flags7 = 0;     // +0x07  bit 0: do not show this id flying in
	int16_t z = 0;          // +0x0A
};

// resolve_item_use_action's two outputs (inventory.c:551-594).
struct UseAction
{
	// 0 = no recipe; 0xFFFFFFFF = effect only; else (first << 16) | second.
	uint32_t packed = 0;
	// The 0x35 effect: an action record id, 0 for none. Meaningful as ROTH.C
	// leaves it -- see the definition for the stale-value case.
	uint32_t effect = 0;
};

// build_inventory_entry_list's result (inventory.c:1314-1368), minus icons.
struct InventoryEntryList
{
	std::vector<int> slots;     // the grid entries, in slot order: slot indices
	int leftEntry = -1;         // g_left_hand_item: index into `slots`, -1 none
	int rightEntry = -1;        // g_right_hand_item
	int adamEntry = -1;         // 0x81048
	int shownLeft = -1;         // g_displayed_item_left: the slot drawn in the left hand box
	int shownRight = -1;        // g_displayed_item_right
};

class Inventory
{
public:
	static const int SLOT_COUNT = 0x100;               // inventory.c:381
	static const uint16_t HIDDEN_BIT = 0x8000;         // inventory.c:1266,1282
	static const int SAVE_BYTES = SLOT_COUNT * 4;      // chunk 7, 0x400
	static const int PICKUP_LOCK_TICKS = 0x1c;         // inventory.c:121

	// reset_inventory (0x1c57e, renderer.c:10256-10261): the count and the 0x400
	// slot bytes, AND NOTHING ELSE. AUDIT: this used to clear the hands and the
	// autoselect flag too; the original leaves both hand pointers standing
	// (new game then runs action 3, restore_active_held_item and
	// activate_weapon_item(0,0), game_core.c:821-831).
	void Reset();

	const InventorySlot &Slot(int i) const { return mSlots[(size_t)i]; }
	int Count() const { return mCount; }
	int LeftHand() const { return mLeft; }
	int RightHand() const { return mRight; }
	int Held() const { return mHeld; }
	int AdamSlot() const { return mAdamSlot; }
	int32_t AdamItemId() const { return mAdamId; }
	bool AutoSelected() const { return mAutoSelected; }
	void ClearAutoSelected() { mAutoSelected = false; }
	bool EasySelect() const { return mEasy; }
	void SetEasySelect(bool on) { mEasy = on; }
	int32_t LastItem() const { return mLastItem; }
	// cmd_give_item / cmd_remove_item write g_last_item_record themselves
	// (raw_commands.c:4827, 4922).
	void SetLastItem(int32_t v) { mLastItem = v; }

	// find_free_inventory_slot (0x1ce43, renderer.c:10030-10041): the FIRST slot
	// whose id word is 0, and it INCREMENTS THE COUNT ITSELF. -1 when all 256
	// are used (the original returns a null pointer, renderer.c:10040).
	int FindFreeSlot();

	// stack_onto_inventory_slot (0x1ce14, renderer.c:10045-10057): the first
	// used slot whose SIGN-EXTENDED id equals `id`, looking at only the first
	// Count() used slots. -1 for none. A hidden slot is negative and never
	// matches a real id -- so giving a duplicate of a hidden stack makes a
	// second slot.
	int FindSlotHolding(int32_t id) const;

	// Hands, set directly (what the savegame restore and tests need).
	void SetLeftHand(int slot) { mLeft = slot; }
	void SetRightHand(int slot, InventorySink *sink = nullptr);

	// give_item (0x1cedc, inventory.c:372-462). Returns the index given,
	// sign-extended from 16 bits, or 0. See the definition.
	int32_t GiveItem(const Dbase100 &db, int32_t index, const GiveContext *ctx = nullptr,
		InventorySink *sink = nullptr);

	// give_item_by_dbase_id (0x1dcef, inventory.c:343-364): world render id ->
	// the first PICKABLE record carrying it -> GiveItem.
	int32_t GiveItemByRenderId(const Dbase100 &db, int32_t renderId, const GiveContext *ctx = nullptr,
		InventorySink *sink = nullptr);

	// query_player_inventory (0x1ccf7, inventory.c:472-531). flags&1 = select
	// mode, flags&2 = never auto-select.
	int Query(const Dbase100 &db, uint16_t id, int flags, InventorySink *sink = nullptr);

	// find_or_autoselect_inventory_item (0x1cb6c, inventory.c:1394-1452): the
	// list form of Query, used by RAW 0x27 in list mode. `ids` are 32-bit, as
	// the RAW id list stores them (raw_commands.c:4768).
	int FindOrAutoselect(const int32_t *ids, int count, int flags, InventoryRng &rng,
		InventorySink *sink = nullptr);

	// remove_item (0x1d077, inventory.c:283-320): right hand, then left hand,
	// then the slot scan. -1 when something was removed, else 0.
	int RemoveItem(const Dbase100 &db, uint16_t id, InventorySink *sink = nullptr);

	// remove_inventory_item (0x1ce6b, inventory.c:246-277) on one slot.
	void RemoveFromSlot(const Dbase100 &db, int slot, InventorySink *sink = nullptr);

	// consume_held_item (0x1d0fd, inventory.c:325-338): remove one slot and
	// rebuild the weapon list. 1 when it ran, 0 for no slot or an empty pack.
	int ConsumeSlot(const Dbase100 &db, int slot, InventorySink *sink = nullptr);

	// set_inventory_list_filter (0x1ca2e, inventory.c:1255-1288), RAW 0x42.
	// Bit 0 clear hides every non-category-2 slot and empties both hands; bit
	// 0 set unhides.
	void SetFilter(const Dbase100 &db, int mode, InventorySink *sink = nullptr);

	// swap_inventory_entries (0x1b007, inventory.c:644-680): `a` is the entry
	// clicked, `b` the entry held on the cursor. See the definition for which
	// trackers follow.
	void SwapSlots(int a, int b, InventorySink *sink = nullptr);

	// --- the cursor-held item (inventory.c section B; dialogue_ui.c:1740-1795) ---

	// Pick a grid cell onto the cursor (dialogue_ui.c:1768-1788): a hand that
	// held that slot is emptied first.
	void PickUpToCursor(int slot, InventorySink *sink = nullptr);
	// A secondary click on the held cell itself (dialogue_ui.c:1745).
	void DropHeld() { mHeld = -1; }
	// Click a hand box with an item held (dialogue_ui.c:1694-1702, 1722-1730).
	void HeldToLeftHand(InventorySink *sink = nullptr);
	void HeldToRightHand(InventorySink *sink = nullptr);
	// commit_held_cursor_item (0x1a88b, inventory.c:2178-2196): the panel
	// closes with an item held. `activeList` is g_cursor_active_list, the
	// CATEGORY of the open tab (TabCategory).
	void CommitHeld(int activeList, InventorySink *sink = nullptr);
	// The Enter key on a grid cell (inventory.c:1710-1720).
	void EnterOnSlot(int activeList, int slot, InventorySink *sink = nullptr);

	// use_item_on_self (0x1b141, inventory.c:691-776), on the held item.
	void UseHeldOnSelf(const Dbase100 &db, InventorySink *sink = nullptr);
	// combine_held_item_with_target (0x1b26d, inventory.c:787-928).
	void CombineHeldWith(const Dbase100 &db, int targetSlot, InventorySink *sink = nullptr);

	// restore_active_held_item (0x1818d, inventory.c:2204-2244): latch the
	// "Adam" record (+5 & 0x10) and fall back to fists if no weapon is equipped.
	void RestoreActiveHeldItem(const Dbase100 &db, InventorySink *sink = nullptr);

	// build_inventory_entry_list (0x19d30, inventory.c:1314-1368), the list
	// part. Clears the held cursor, as the original does first (:1317).
	InventoryEntryList BuildEntryList(const Dbase100 &db, int category, InventorySink *sink = nullptr);

	// --- the pickup lock (inventory.c:32-37, 106-139; renderer.c:9880-9887) ---
	// Armed, it refuses further pickups (input.c:1055). GiveItem arms it when
	// it starts a fly-in.
	bool PickupLockArmed() const { return (mPickupFlags & 1) != 0; }
	uint32_t PickupLockFlags() const { return mPickupFlags; }
	int32_t PickupLockTimer() const { return mPickupTimer; }
	void BeginPickupLock() { mPickupFlags |= 5; }               // renderer.c:9882
	// tick_item_pickup_lock's timer: += step; at 0x1c or more, disarm.
	void TickPickupLock(int32_t step);
	// reset_item_pickup_lock (0x18003): the flags only -- NOT the timer.
	void ResetPickupLock() { mPickupFlags = 0; }

	// Save chunk 7: the slot array raw, hidden bits and stale quantities
	// included (savegame.c:1592-1593). Loading recomputes the count from the
	// non-zero id words and rebuilds the weapon list (savegame.c:1410-1417).
	std::vector<uint8_t> Serialize() const;
	bool Deserialize(const uint8_t *data, size_t size, InventorySink *sink = nullptr);

	// Save chunk 2's two hand fields (savegame.c:152-157, 187-192): the byte
	// offset of the slot from the array base, 0 FOR NONE -- so a hand on slot 0
	// saves as "none" and comes back empty. That is the original's behaviour.
	void SaveHands(uint32_t &leftOut, uint32_t &rightOut) const;
	void LoadHands(uint32_t left, uint32_t right);

private:
	int32_t SignedId(int slot) const { return (int16_t)mSlots[slot].id; }
	void EquipLeft(int slot, int32_t itemId, InventorySink *sink);

	InventorySlot mSlots[SLOT_COUNT];
	int mCount = 0;
	int mLeft = -1, mRight = -1;
	int mHeld = -1;
	int mAdamSlot = -1;
	int32_t mAdamId = 0;
	bool mAutoSelected = false;
	bool mEasy = false;
	int32_t mLastItem = 0;
	uint32_t mPickupFlags = 0;
	int32_t mPickupTimer = 0;
};

// is_item_id_pickable (0x1dd50, inventory.c:44-65), on the WORLD id -- the
// object's textureIndex | textureSource << 8. The FIRST record carrying the id
// decides. AUDIT: the id is 32-bit and compared with the record's
// SIGN-EXTENDED +2 word; it was uint16_t.
bool IsItemIdPickable(const Dbase100 &db, int32_t renderId);

// resolve_item_use_action (0x18060, inventory.c:551-594): the OnUse recipe
// matcher. `target` is 0 for use-on-self, else the partner's item id.
UseAction ResolveItemUseAction(const Dbase100 &db, const DbItem *rec, int32_t target);

// The quantity give_item stores for a weapon: WeaponAction attrs+4, shifted
// left 8 when attrs+0x31 & 0x80 (inventory.c:392-396 over
// apply_weapon_action_attributes, weapon_combat.c:338-420). 32-bit; the slot
// keeps the low 16.
int32_t WeaponGiveQuantity(const Dbase100 &db, const DbItem &rec);
// attrs+0x31 & 0x80, the "quantity is rounds << 8" flag (op 0x20, bit 15).
bool WeaponQuantityIsShifted(const Dbase100 &db, const DbItem &rec);

// node_has_available_choice (0x1fa91, dialogue_ui.c:74-97): does a category-2
// record have a topic to offer? Decides whether it is listed (inventory.c:1335).
bool ItemHasAvailableChoice(const Dbase100 &db, int32_t itemId, InventorySink *sink);

// scan_tag4_chunk (0x1dda8, renderer.c:1485-1502): the OnInspect block of a
// record that carries +4 & 0x40, or null.
const DbTriggerBlock *FindInspectBlock(const DbItem &rec);
// find_oninspect_block_by_id (0x1ddeb, dialogue_ui.c:518-539), without its
// one-entry cache: world render id -> item id whose OnInspect block a
// right-click runs. 0 for none. `blockOut` receives the block.
int32_t FindInspectByRenderId(const Dbase100 &db, int32_t renderId, const DbTriggerBlock **blockOut);

// The first-examine rule (file_config.c:962-967): if record +7 bit 0x80 is
// clear, SET IT FIRST, then run the OnInspect block (mode 0). True when this
// was the first view. The bit lives in the DBASE100 image (SetItemRuntime).
bool ExamineFirstView(Dbase100 &db, int32_t itemId, InventorySink *sink);
// The magnifier button (dialogue_ui.c:1569-1573): run OnInspect again.
void ReplayInspect(const Dbase100 &db, int32_t itemId, InventorySink *sink);

// format_inventory_item_label's text choice (0x1c0b1, inventory.c:1031-1039):
// record +0x10, unless resolve_record_conditional_op2 (inventory.c:73-97)
// finds a ChangeName block whose condition holds -- then that block's first
// command operand. A DBASE400 offset. 0 for item 0.
uint32_t ItemLabelText(const Dbase100 &db, int32_t itemId, InventorySink *sink);
// The quantity draw_inventory_entry_label shows (inventory.c:1573-1584): the
// slot quantity sign-extended, logically shifted right 8 for a weapon whose
// quantity is rounds << 8. The label is the bare name when this is 0, else
// the format at 0x75e2c, "%s (%D)" (obj3_owned.c g_heap_free_list +0x81c),
// both clipped to 0x61 bytes of name (inventory.c:1047, 1057).
int32_t ItemLabelQuantity(const Dbase100 &db, const InventorySlot &slot);

// The five tab categories, 0x7123c: {0, 1, 3, 4, 2} (obj3_owned.c
// g_inventory_tab_context_map, first five bytes). select_inventory_tab maps
// the active tab through this to the list category (inventory.c:1560-1563).
int TabCategory(int tab);
// get_item_tab_index (0x1b0b2, inventory.c:2153-2161): the tab an item lives
// on; 0 for item 0 or an unlisted category.
int ItemTabIndex(const Dbase100 &db, int32_t itemId);

// NOT HERE, AND WHY -- the full list is at the end of roth_inventory.cpp.

} // namespace roth
