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

class GameCore
{
public:
	GameCore();

	// Point the core at a loaded database. `interpHost` and `invHost` receive
	// every call that belongs to another system; either may be null.
	void Attach(const Dbase100 *db, Dbase100Sink *interpHost = nullptr, InventorySink *invHost = nullptr);
	bool IsAttached() const { return mDb != nullptr; }

	Dbase100Interp &Interp() { return mInterp; }
	Inventory &Items() { return mInventory; }
	RothIndexRng &Rng() { return mRng; }
	const Dbase100 *Db() const { return mDb; }

	// The four fixed action records (game_core.c:757, 802, 825, 971).
	enum { ACTION_BOOT = 1, ACTION_QUIT = 2, ACTION_NEW_GAME = 3, ACTION_DEATH = 4 };

	// A new game as far as these two systems go: an empty inventory, cleared
	// flags and dialogue state, then action 3. [unverified]: the order and the
	// full list of what ROTH.C resets around action 3 (game_core.c:825) has
	// not been transcribed -- the player, the map and the weapon state are
	// other systems and are not touched here.
	uint32_t NewGame();

	// RAW 0x2b: run an action record by id, then finish the evaluation.
	uint32_t RunAction(uint32_t id);

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

	const Dbase100 *mDb = nullptr;
	Dbase100Interp mInterp;
	Inventory mInventory;
	RothIndexRng mRng;
	InterpBridge mInterpBridge;
	InvBridge mInvBridge;
};

} // namespace roth
