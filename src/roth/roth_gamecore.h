#pragma once
//
// The Realms game core: one object that owns the rule state and joins the
// pieces that call each other.
//
// ROTH.C's DBASE100 interpreter and its inventory are not layers, they are
// peers: the interpreter gives, removes and counts items (dbase100.c:770-771,
// 813, 823), and the inventory runs interpreter chains for use, combine and
// examine (inventory.c:760-773, 914-927). They also draw from ONE random
// generator (dbase100.c:753; renderer.c:11130-11147). Each module was ported
// behind its own sink so it could be tested alone; this is where the two
// sinks are pointed at each other.
//
// Everything that is neither interpreter nor inventory -- text, voice,
// cutscenes, sound, music, the RAW command chains, health, weapons, the
// panel -- still leaves through a sink: the two host pointers below. With
// no host, those calls do nothing, which is what the engine-free tests use.
//
// This is ROTH_GAME_PORT.md section 2.1's "one Realms core" in its first,
// smallest form. Standard library only; no engine headers.
//

#include "roth_dbase100.h"
#include "roth_dbase100_interp.h"
#include "roth_inventory.h"

namespace roth
{

// Watches the calls the interpreter and the inventory make INTO EACH OTHER
// (and into the shared generator) inside the core, where no host sink sees
// them. For the differential test against the original (tools/gameproof);
// the engine does not set one.
class GameCoreObserver
{
public:
	virtual ~GameCoreObserver() {}
	// `what` is one of "give", "remove", "query", "find", "filter",
	// "evalblock" (a, b = the call's two arguments) or "rng" (a = count).
	virtual void Enter(const char *what, int32_t a, int32_t b) { (void)what; (void)a; (void)b; }
	virtual void Leave(const char *what, int32_t ret) { (void)what; (void)ret; }
};

class GameCore
{
public:
	GameCore();

	void SetObserver(GameCoreObserver *o) { mObserver = o; }

	// Point the core at a loaded database. `interpHost` and `invHost` receive
	// every call that belongs to another system; either may be null.
	void Attach(const Dbase100 *db, Dbase100Sink *interpHost = nullptr, InventorySink *invHost = nullptr);
	bool IsAttached() const { return mDb != nullptr; }

	Dbase100Interp &Interp() { return mInterp; }
	Inventory &Items() { return mInventory; }
	RothIndexRng &Rng() { return mRng; }
	const Dbase100 *Db() const { return mDb; }
	// The inventory's sink as the core wires it (interpreter chains answered by
	// the core, the rest forwarded to the host): what a caller of an Inventory
	// method outside the core must pass, so the same systems hear of it.
	InventorySink *InvSink() { return &mInvBridge; }

	// The four fixed action records (game_core.c:757, 802, 825, 971).
	enum { ACTION_BOOT = 1, ACTION_QUIT = 2, ACTION_NEW_GAME = 3, ACTION_DEATH = 4 };

	// A new game as far as these two systems go, in the original's order
	// (game_core.c:821-833): cleared flags and dialogue state, an empty
	// inventory, action 3, the Adam latch restored, back to fists. The
	// player's health and position and the map are other systems.
	uint32_t NewGame();

	// RAW 0x2b: run an action record by id, then finish the evaluation.
	uint32_t RunAction(uint32_t id);

	// The inventory entries the RAW commands call, each going out through the
	// core's own sink so that weapons, the panel and the interpreter hear of it.
	int32_t Give(int32_t item, const GiveContext *ctx = nullptr);        // give_item, RAW 0x29
	int Query(uint16_t id, int flags);                                   // query_player_inventory, RAW 0x27
	int FindOrAutoselect(const int32_t *ids, int count, int flags);      // RAW 0x27, list mode
	int Remove(uint16_t id);                                             // remove_item, RAW 0x2a
	void SetFilter(int mode);                                            // set_inventory_list_filter, RAW 0x42

	// For the game flow (roth_gameflow): the sink the inventory's own calls
	// leave through, so a restore or a boot reaches weapons and the panel the
	// way a give does.
	InventorySink *ItemSink() { return &mInvBridge; }
	// activate_weapon_item(0, 0): back to fists (game_core.c:832, 962).
	void UnequipWeapon();
	// activate_weapon_item(weapon hand): what a load does once the slots and
	// the hands are back (savegame.c:1489-1493).
	void ReactivateWeapon();

private:
	// The interpreter's calls: inventory and RNG are answered here, the rest
	// goes to the host.
	class InterpBridge : public Dbase100Sink
	{
	public:
		explicit InterpBridge(GameCore &c) : core(c) {}
		GameCore &core;
		Dbase100Sink *host = nullptr;

		uint32_t GiveItem(int32_t id, uint32_t ctx) override;
		uint32_t RemoveItem(int32_t id) override;
		uint32_t QueryPlayerInventory(int32_t id, uint32_t flags) override;
		int32_t RngNextIndexForCount(int32_t count) override;

		uint32_t ReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t voiceOffset, uint32_t flag) override;
		uint32_t Dbase100OpenDialogueWindow(const uint8_t *text, uint32_t bytes) override;
		uint32_t Dbase100OpenDialogueWindowAlt(const uint8_t *text, uint32_t bytes) override;
		void DialogueVoiceForceEnd(uint32_t eax, uint32_t edx) override;
		uint32_t PlayRecordGdvCutscene(int index) override;
		uint32_t ShowFullscreenImage(uint32_t operand) override;
		void ExitCutsceneOverlayMode() override;
		uint32_t PlaySoundEffect(uint32_t id, uint32_t param) override;
		uint32_t LoadDbase300Chunk(uint32_t index) override;
		uint32_t FinalizeAudioSequenceRef() override;
		uint32_t EmitMusicSequenceEvent(uint32_t event) override;
		void ProcessAudioSequenceChunk() override;
		int32_t ResetCommandChainNoSource(uint16_t index) override;
		int32_t RunObjectCommandsById(uint16_t id) override;
		void AdvanceClamp8a0f0(uint32_t amount, uint32_t max) override;
		int32_t AudioSeqState() override;
		void SetAudioSeqState(int32_t v) override;
		uint32_t CurrentDbase300ChunkId() override;
		uint8_t MusicMasterVolume() override;
		int32_t CutsceneOverlayActive() override;
		int32_t InventoryPanelOpen() override;
		int32_t InspectPopupActive() override;
		void ArmInventoryPanelExit() override;
		void SetLatch83b20() override;
		void SetLatch80b2c() override;
		void ZeroHealthAndFlagDeath() override;
		uint32_t HealthClampMax() override;
	};

	// The inventory's calls: interpreter chains are answered here, the rest
	// goes to the host.
	class InvBridge : public InventorySink
	{
	public:
		explicit InvBridge(GameCore &c) : core(c) {}
		GameCore &core;
		InventorySink *host = nullptr;

		int32_t EvalItemBlock(int32_t itemId, const DbTriggerBlock &block, int mode) override;
		void RunActionRecord(uint32_t actionId) override;

		void UnequipWeapon() override;
		void EquipWeapon(int slot, int32_t itemId) override;
		void WeaponHudDirty() override;
		void RebuildWeaponList() override;
		void ResetWeaponHud() override;
		bool IsActiveAmmo(int32_t itemId) override;
		void AmmoDisplayChanged(int delta) override;
		void RightHandChanged(int slot) override;
		void HeldItemSlideReset() override;
		void ClearTabCursors() override;
		void RefreshPanel() override;
		bool PanelCloseRequested() override;
		void PickupFlyStarted(int32_t x, int32_t y, int32_t z, uint16_t renderId) override;
		void ItemMessage(int interfaceIndex, int colorIndex, uint32_t nameA, uint32_t nameB) override;
	};

	int32_t DrawIndex(int32_t count);
	// The generator as the inventory takes it (InventoryRng), through DrawIndex.
	class RngTap : public InventoryRng
	{
	public:
		explicit RngTap(GameCore &c) : core(c) {}
		GameCore &core;
		int32_t NextIndexForCount(int32_t count) override { return core.DrawIndex(count); }
	};
	void Enter(const char *what, int32_t a, int32_t b) { if (mObserver != nullptr) mObserver->Enter(what, a, b); }
	void Leave(const char *what, int32_t ret) { if (mObserver != nullptr) mObserver->Leave(what, ret); }

	GameCoreObserver *mObserver = nullptr;
	const Dbase100 *mDb = nullptr;
	Dbase100Interp mInterp;
	Inventory mInventory;
	RothIndexRng mRng;
	InterpBridge mInterpBridge;
	InvBridge mInvBridge;
};

} // namespace roth
