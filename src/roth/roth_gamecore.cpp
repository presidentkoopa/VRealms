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
	// Attach re-zeroes the flag bitmap and the dialogue state, exactly as
	// init_game_databases leaves them (game_core.c:420-432). The generator is
	// NOT reseeded: its state is fixed from program start, not from a new game
	// (roth_inventory.h, RothIndexRng).
	mInterp.Attach(mDb, &mInterpBridge);
	mInventory.Reset();
	return RunAction(ACTION_NEW_GAME);
}

uint32_t GameCore::RunAction(uint32_t id)
{
	if (mDb == nullptr) return 0;
	const uint32_t r = mInterp.EvalDialogueRecordById(id);     // dbase100.c:545-555
	mInterp.FinishDialogueRecordEval();                         // dbase100.c:291-296
	return r;
}

int32_t GameCore::Give(int32_t item, const GiveContext *ctx)
{
	if (mDb == nullptr) return 0;
	return mInventory.GiveItem(*mDb, item, ctx, &mInvBridge);
}

int GameCore::Query(uint16_t id, int flags)
{
	if (mDb == nullptr) return 0;
	return mInventory.Query(*mDb, id, flags, &mInvBridge);
}

int GameCore::FindOrAutoselect(const int32_t *ids, int count, int flags)
{
	if (mDb == nullptr) return 0;
	return mInventory.FindOrAutoselect(ids, count, flags, mRng, &mInvBridge);
}

int GameCore::Remove(uint16_t id)
{
	if (mDb == nullptr) return 0;
	return mInventory.RemoveItem(*mDb, id, &mInvBridge);
}

void GameCore::SetFilter(int mode)
{
	if (mDb == nullptr) return;
	mInventory.SetFilter(*mDb, mode, &mInvBridge);
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
	if (core.mDb == nullptr) return 0;
	return (uint32_t)core.mInventory.GiveItem(*core.mDb, id, nullptr, &core.mInvBridge);
}

uint32_t GameCore::InterpBridge::RemoveItem(int32_t id)
{
	if (core.mDb == nullptr) return 0;
	return (uint32_t)core.mInventory.RemoveItem(*core.mDb, (uint16_t)id, &core.mInvBridge);
}

uint32_t GameCore::InterpBridge::QueryPlayerInventory(int32_t id, uint32_t flags)
{
	if (core.mDb == nullptr) return 0;
	return (uint32_t)core.mInventory.Query(*core.mDb, (uint16_t)id, (int)flags, &core.mInvBridge);
}

int32_t GameCore::InterpBridge::RngNextIndexForCount(int32_t count)
{
	return core.mRng.NextIndexForCount(count);
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
	return (int32_t)core.mInterp.EvalBlock(block, (uint32_t)mode);
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
