//
// The Realms game core: the interpreter and the inventory, pointed at each
// other. See roth_gamecore.h.
//

#include "roth_gamecore.h"

namespace roth
{

GameCore::GameCore() : mInterpBridge(*this), mInvBridge(*this)
{
}

void GameCore::Attach(const Dbase100 *db, Dbase100Sink *interpHost, InventorySink *invHost)
{
	mDb = (db != nullptr && db->IsLoaded()) ? db : nullptr;
	mInterpBridge.host = interpHost;
	mInvBridge.host = invHost;
	mInterp.Attach(mDb, &mInterpBridge);
	mInventory.Reset();
	mRng = RothIndexRng();
}

uint32_t GameCore::NewGame()
{
	if (mDb == nullptr) return 0;
	// The new-game block of the main loop, game_core.c:821-833, as far as these
	// two systems go. In its order:
	//   clear_dual_array_80afc, reset_inventory            :822-823
	//   eval_dialogue_record_by_id(3), finish               :824-826
	//   restore_active_held_item                            :827
	//   (health = 0x800 when unset -- the player's, not here) :828-830
	//   activate_weapon_item(0, 0)                           :832
	// The flag bitmap is zeroed at a new game too (game_core.c:459); Attach
	// does that and resets the dialogue state. The generator is NOT reseeded:
	// its state is fixed from program start (roth_inventory.h, RothIndexRng).
	mInterp.Attach(mDb, &mInterpBridge);
	mInvBridge.ClearTabCursors();
	mInventory.Reset();
	const uint32_t r = RunAction(ACTION_NEW_GAME);
	mInventory.RestoreActiveHeldItem(*mDb, &mInvBridge);
	mInventory.SetLeftHand(-1);
	mInvBridge.UnequipWeapon();
	return r;
}

uint32_t GameCore::RunAction(uint32_t id)
{
	if (mDb == nullptr) return 0;
	const uint32_t r = mInterp.EvalDialogueRecordById(id);     // dbase100.c:545-555
	mInterp.FinishDialogueRecordEval();                         // dbase100.c:291-296
	return r;
}

// Every draw from the shared generator goes through here (dbase100.c:753,
// inventory.c:1432), so the observer sees each one.
int32_t GameCore::DrawIndex(int32_t count)
{
	const int32_t r = mRng.NextIndexForCount(count);
	if (mObserver != nullptr) { mObserver->Enter("rng", count, 0); mObserver->Leave("rng", r); }
	return r;
}

int32_t GameCore::Give(int32_t item, const GiveContext *ctx)
{
	if (mDb == nullptr) return 0;
	Enter("give", item, ctx != nullptr);
	const int32_t r = mInventory.GiveItem(*mDb, item, ctx, &mInvBridge);
	Leave("give", r);
	return r;
}

int GameCore::Query(uint16_t id, int flags)
{
	if (mDb == nullptr) return 0;
	Enter("query", (int16_t)id, flags);
	const int r = mInventory.Query(*mDb, id, flags, &mInvBridge);
	Leave("query", r);
	return r;
}

int GameCore::FindOrAutoselect(const int32_t *ids, int count, int flags)
{
	if (mDb == nullptr) return 0;
	Enter("find", count, flags);
	RngTap tap(*this);
	const int r = mInventory.FindOrAutoselect(ids, count, flags, tap, &mInvBridge);
	Leave("find", r);
	return r;
}

int GameCore::Remove(uint16_t id)
{
	if (mDb == nullptr) return 0;
	Enter("remove", (int16_t)id, 0);
	const int r = mInventory.RemoveItem(*mDb, id, &mInvBridge);
	Leave("remove", r);
	return r;
}

void GameCore::SetFilter(int mode)
{
	if (mDb == nullptr) return;
	Enter("filter", mode, 0);
	mInventory.SetFilter(*mDb, mode, &mInvBridge);
	Leave("filter", 0);
}

void GameCore::UnequipWeapon()
{
	mInventory.SetLeftHand(-1);
	mInvBridge.UnequipWeapon();
}

void GameCore::ReactivateWeapon()
{
	// savegame.c:1489-1493: the slot in the weapon hand and its item id,
	// sign-extended; an empty hand is activate_weapon_item(0, 0).
	const int slot = mInventory.LeftHand();
	if (slot < 0) { mInvBridge.UnequipWeapon(); return; }
	mInvBridge.EquipWeapon(slot, (int32_t)(int16_t)mInventory.Slot(slot).id);
}

//==========================================================================
//
// Interpreter -> inventory, and the shared generator.
//
//==========================================================================

uint32_t GameCore::InterpBridge::GiveItem(int32_t id, uint32_t ctx)
{
	// Op 0x11 always passes a null context (dbase100.c:770).
	(void)ctx;
	return (uint32_t)core.Give(id, nullptr);
}

uint32_t GameCore::InterpBridge::RemoveItem(int32_t id)
{
	return (uint32_t)core.Remove((uint16_t)id);
}

uint32_t GameCore::InterpBridge::QueryPlayerInventory(int32_t id, uint32_t flags)
{
	return (uint32_t)core.Query((uint16_t)id, (int)flags);
}

int32_t GameCore::InterpBridge::RngNextIndexForCount(int32_t count)
{
	return core.DrawIndex(count);
}

// Everything else belongs to a system that is not in the core yet.
#define FWD(expr, fallback) do { if (host != nullptr) return host->expr; return fallback; } while (0)
#define FWDV(expr) do { if (host != nullptr) host->expr; } while (0)

uint32_t GameCore::InterpBridge::ReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t voiceOffset, uint32_t flag)
{ FWD(ReadNextDialogueLine(dest, maxlen, voiceOffset, flag), Dbase100Sink::ReadNextDialogueLine(dest, maxlen, voiceOffset, flag)); }
uint32_t GameCore::InterpBridge::Dbase100OpenDialogueWindow(const uint8_t *text, uint32_t bytes) { FWD(Dbase100OpenDialogueWindow(text, bytes), 0); }
uint32_t GameCore::InterpBridge::Dbase100OpenDialogueWindowAlt(const uint8_t *text, uint32_t bytes) { FWD(Dbase100OpenDialogueWindowAlt(text, bytes), 0); }
void GameCore::InterpBridge::DialogueVoiceForceEnd(uint32_t eax, uint32_t edx) { FWDV(DialogueVoiceForceEnd(eax, edx)); }
uint32_t GameCore::InterpBridge::PlayRecordGdvCutscene(int index) { FWD(PlayRecordGdvCutscene(index), 0); }
uint32_t GameCore::InterpBridge::ShowFullscreenImage(uint32_t operand) { FWD(ShowFullscreenImage(operand), 0); }
void GameCore::InterpBridge::ExitCutsceneOverlayMode() { FWDV(ExitCutsceneOverlayMode()); }
uint32_t GameCore::InterpBridge::PlaySoundEffect(uint32_t id, uint32_t param) { FWD(PlaySoundEffect(id, param), 0); }
// With no music system the chunk reads as already loaded, as the bare sink does.
uint32_t GameCore::InterpBridge::LoadDbase300Chunk(uint32_t index) { FWD(LoadDbase300Chunk(index), 1); }
uint32_t GameCore::InterpBridge::FinalizeAudioSequenceRef() { FWD(FinalizeAudioSequenceRef(), 0); }
uint32_t GameCore::InterpBridge::EmitMusicSequenceEvent(uint32_t event) { FWD(EmitMusicSequenceEvent(event), 0); }
void GameCore::InterpBridge::ProcessAudioSequenceChunk() { FWDV(ProcessAudioSequenceChunk()); }
int32_t GameCore::InterpBridge::ResetCommandChainNoSource(uint16_t index) { FWD(ResetCommandChainNoSource(index), 0); }
int32_t GameCore::InterpBridge::RunObjectCommandsById(uint16_t id) { FWD(RunObjectCommandsById(id), 0); }
void GameCore::InterpBridge::AdvanceClamp8a0f0(uint32_t amount, uint32_t max) { FWDV(AdvanceClamp8a0f0(amount, max)); }
int32_t GameCore::InterpBridge::AudioSeqState() { FWD(AudioSeqState(), 0); }
void GameCore::InterpBridge::SetAudioSeqState(int32_t v) { FWDV(SetAudioSeqState(v)); }
uint32_t GameCore::InterpBridge::CurrentDbase300ChunkId() { FWD(CurrentDbase300ChunkId(), 0); }
uint8_t GameCore::InterpBridge::MusicMasterVolume() { FWD(MusicMasterVolume(), 0); }
int32_t GameCore::InterpBridge::CutsceneOverlayActive() { FWD(CutsceneOverlayActive(), 0); }
int32_t GameCore::InterpBridge::InventoryPanelOpen() { FWD(InventoryPanelOpen(), 0); }
int32_t GameCore::InterpBridge::InspectPopupActive() { FWD(InspectPopupActive(), 0); }
void GameCore::InterpBridge::ArmInventoryPanelExit() { FWDV(ArmInventoryPanelExit()); }
void GameCore::InterpBridge::SetLatch83b20() { FWDV(SetLatch83b20()); }
void GameCore::InterpBridge::SetLatch80b2c() { FWDV(SetLatch80b2c()); }
void GameCore::InterpBridge::ZeroHealthAndFlagDeath() { FWDV(ZeroHealthAndFlagDeath()); }
uint32_t GameCore::InterpBridge::HealthClampMax() { FWD(HealthClampMax(), 0); }

//==========================================================================
//
// Inventory -> interpreter.
//
//==========================================================================

int32_t GameCore::InvBridge::EvalItemBlock(int32_t itemId, const DbTriggerBlock &block, int mode)
{
	(void)itemId;
	// eval_dialogue_record_condition_with_cleanup(block, mode), dbase100.c:569-576.
	core.Enter("evalblock", (int32_t)block.offset, mode);
	const int32_t r = (int32_t)core.mInterp.EvalBlock(block, (uint32_t)mode);
	core.Leave("evalblock", r);
	return r;
}

void GameCore::InvBridge::RunActionRecord(uint32_t actionId)
{
	// eval_dialogue_record_by_id + finish_dialogue_record_eval
	// (inventory.c:760-773, 914-927).
	core.RunAction(actionId);
}

void GameCore::InvBridge::UnequipWeapon() { FWDV(UnequipWeapon()); }
void GameCore::InvBridge::EquipWeapon(int slot, int32_t itemId) { FWDV(EquipWeapon(slot, itemId)); }
void GameCore::InvBridge::WeaponHudDirty() { FWDV(WeaponHudDirty()); }
void GameCore::InvBridge::RebuildWeaponList() { FWDV(RebuildWeaponList()); }
void GameCore::InvBridge::ResetWeaponHud() { FWDV(ResetWeaponHud()); }
bool GameCore::InvBridge::IsActiveAmmo(int32_t itemId) { FWD(IsActiveAmmo(itemId), false); }
void GameCore::InvBridge::AmmoDisplayChanged(int delta) { FWDV(AmmoDisplayChanged(delta)); }
void GameCore::InvBridge::RightHandChanged(int slot) { FWDV(RightHandChanged(slot)); }
void GameCore::InvBridge::HeldItemSlideReset() { FWDV(HeldItemSlideReset()); }
void GameCore::InvBridge::ClearTabCursors() { FWDV(ClearTabCursors()); }
void GameCore::InvBridge::RefreshPanel() { FWDV(RefreshPanel()); }
bool GameCore::InvBridge::PanelCloseRequested() { FWD(PanelCloseRequested(), false); }
void GameCore::InvBridge::PickupFlyStarted(int32_t x, int32_t y, int32_t z, uint16_t renderId) { FWDV(PickupFlyStarted(x, y, z, renderId)); }
void GameCore::InvBridge::ItemMessage(int interfaceIndex, int colorIndex, uint32_t nameA, uint32_t nameB) { FWDV(ItemMessage(interfaceIndex, colorIndex, nameA, nameB)); }

#undef FWD
#undef FWDV

} // namespace roth
