#pragma once
//
// The RAW map commands that reach the game core: 0x27 (if-not-item), 0x29
// (give item), 0x2a (remove item), 0x2b (run a DBASE100 action) and 0x42
// (inventory filter), with the two continue-tails the conditional cluster
// shares.
//
// Engine-free, so the same code the level runs is the code the differential
// test runs against the original (tools/gameproof): each function below is
// called on record bytes from the retail maps and compared, call by call,
// with ROTH.C's cmd_* of the same name running in the original.
//
// Rules: roth_c/src/engine/raw_commands.c ("raw_commands.c:LINE").
//
// A record here is the three fields these commands read or write. The level
// owns the real record; it copies `modifier` back after the call.
//

#include "roth_gamecore.h"

namespace roth
{

struct ItemCmdRecord
{
	uint8_t modifier = 0;       // rec+2; bit 0x08 = "done, skip from now on"
	uint8_t fireFlags = 0;      // rec+6
	uint16_t key = 0;           // word rec+8
};

// The chain state these commands share (raw_commands.c globals).
struct ItemCmdState
{
	// g_pending_command_record 0x8a0dc: the 0x2b record an earlier command of
	// the chain latched to run "if the next condition holds". ROTH.C keeps a
	// record pointer; this keeps whatever handle the host resolves, 0 = none.
	uint32_t pendingRecord = 0;
	// g_command_chain_interrupt 0x8a268: 1 = stop the chain, 2 = stop and
	// re-run from the latched index.
	uint8_t interrupt = 0;
	// 0x8a26c: set by the first 0x2b of a chain (raw_commands.c:4701-4704).
	bool dialogueLatch = false;
	// g_if_item_list 0x89f68 and its count 0x89f64 (raw_commands.c:4765-4770).
	int32_t list[16] = {};
	int listCount = 0;
};

class ItemCmdHost
{
public:
	virtual ~ItemCmdHost() {}
	// The record behind ItemCmdState::pendingRecord, or null.
	virtual ItemCmdRecord *PendingRecord(uint32_t handle) = 0;
	// Called after a pending record ran, so the host can copy its modifier back.
	virtual void PendingRecordRan(uint32_t handle) { (void)handle; }
};

// Return values are ROTH.C's: 0 = did nothing, -1 = acted. TailIfNext can also
// return a positive value: the still-latched pending record (ROTH.C returns
// its pointer, raw_commands.c:4651-4658).

// rawcmd_tail_consume, raw_commands.c:4637-4643.
int32_t ItemCmdTailConsume(ItemCmdState &st, ItemCmdRecord &rec, int32_t ret);
// rawcmd_tail_ifnext, raw_commands.c:4648-4659.
int32_t ItemCmdTailIfNext(GameCore &core, ItemCmdState &st, ItemCmdHost &host);

// run_command_dbase100_record (0x2b), raw_commands.c:4697-4709.
int32_t RunCommandDbase100Record(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec);
// cmd_if_not_item (0x27), raw_commands.c:4760-4789.
int32_t CmdIfNotItem(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec, ItemCmdHost &host);
// cmd_give_item (0x29), raw_commands.c:4799-4830. `ctx` is the context the
// level worked out (:4801-4822), or null.
int32_t CmdGiveItem(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec, const GiveContext *ctx);
// cmd_remove_item (0x2a), raw_commands.c:4918-4931.
int32_t CmdRemoveItem(GameCore &core, ItemCmdState &st, ItemCmdRecord &rec);
// cmd_set_inventory_filter (0x42), raw_commands.c:4940-4947.
int32_t CmdSetInventoryFilter(GameCore &core, ItemCmdRecord &rec);

} // namespace roth
