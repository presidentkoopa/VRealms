//
// The command spine, transcribed from ROTH.C.
//
// Two functions, both faithful to the original's control flow rather than a
// reconstruction of what it probably meant:
//
//   WalkChainFlow  <- walk_command_chain_flow   (raw_commands.c:1094)
//   ExecChain      <- rawcmd_exec_loop          (raw_commands.c:1229)
//
// The original resolves a record through resolve_command_by_index
// (renderer.c:9830), which returns base[(index - 1) * 4] and treats index 0 as
// "none" -- hence the 1-based indexing throughout.
//

#include "roth_commands.h"
#include "roth_raw.h"

namespace roth
{

// Gate opcodes. These are the ONLY two the flow pre-pass looks at.
static const uint8_t OP_DELAY_TIMER  = 0x12;
static const uint8_t OP_MODIFY_COUNT = 0x1e;

// Modifier bit 0x08 = disabled. Every scanner in the original tests it.
static const uint8_t MOD_DISABLED = 0x08;

// Recursion bound for the 0x1e sub-chain walk. The original has none and relies
// on the data being sane; a malformed chain would loop forever here, and a hang
// is a worse failure than a bounded bail.
static const int MAX_FLOW_DEPTH = 64;

static const Command *Resolve(const Map &map, uint16_t index)
{
	if (index == 0) return nullptr;                       // 0 means "none"
	const size_t i = (size_t)(index - 1);
	if (i >= map.commands.size()) return nullptr;         // see the note below
	return &map.commands[i];
}

//==========================================================================
//
// WalkChainFlow -- walk_command_chain_flow (raw_commands.c:1094)
//
// Follows NEXT links and decides whether the chain is gated, running only the
// two flow opcodes:
//
//   0x12 Delay Timer   blocks when modifier bit 0x04 is CLEAR and 0x01|0x20 set
//   0x1e Modify Count  recurses into the sub-chain at its key, and blocks if
//                      that sub-walk blocks
//
// An out-of-range index is treated as "chain OK". That matches what the
// original actually does in practice: on flat DOS an OOB index reads adjacent
// heap as a record pointer, whose garbage opcode almost never equals one of the
// two gate opcodes, so the walk chases links to a terminating zero and reports
// the chain clear. ROTH.C carries an explicit guard for the same reason, since
// on a modern host that read faults instead.
//
//==========================================================================

static uint16_t WalkFlow(const Map &map, uint16_t index, int depth)
{
	for (;;)
	{
		if (index == 0) return 0;
		const Command *rec = Resolve(map, index);
		if (rec == nullptr) return 0;                     // OOB -> chain OK

		if (rec->opcode == OP_DELAY_TIMER
			&& !(rec->modifier & 0x04) && (rec->modifier & 0x21))
			return index;                                 // gated here

		if (rec->opcode == OP_MODIFY_COUNT && rec->key != 0)
		{
			if (depth < MAX_FLOW_DEPTH && WalkFlow(map, rec->key, depth + 1) != 0)
				return index;                             // sub-chain blocked
		}

		index = rec->linkIndex;                           // follow NEXT
	}
}

uint16_t WalkChainFlow(const Map &map, uint16_t index)
{
	return WalkFlow(map, index, 0);
}

//==========================================================================
//
// ExecChain -- rawcmd_exec_loop (raw_commands.c:1229)
//
// The control flow, in the original's own order:
//
//   - a record whose modifier has 0x08 is SKIPPED but its NEXT is still
//     followed, so a disabled record does not break the chain
//   - the dispatch index is `opcode & 0x7f`. That mask is where the 0x80 bit
//     goes: it is authoring metadata from the level editor, carried on about a
//     third of all records and never tested at runtime
//   - a handler's result is OR-ed into the chain's accumulated result
//   - a handler may interrupt, which stops the chain immediately
//   - **0x12 Delay Timer always stops the chain**, whether or not it acted
//   - otherwise follow NEXT until it is zero
//
// After the chain ends, an interrupt of Rerun with a latched next-active index
// restarts execution from there -- that is how 0x38 ("jump if the next step
// fails") gets its second attempt.
//
//==========================================================================

int ExecChain(const Map &map, uint16_t index, Handlers &h)
{
	int result = CMD_NOTHING;
	uint16_t savedNext = 0;

	for (;;)
	{
		for (;;)
		{
			if (index == 0) break;
			const Command *rec = Resolve(map, index);
			if (rec == nullptr) break;                    // OOB -> end the chain

			if (!(rec->modifier & MOD_DISABLED))
			{
				// The original latches the previous next-active and clears it
				// before every dispatch, so a handler only sees what IT set.
				savedNext = h.nextActive;
				h.nextActive = 0;

				if (h.Run) result |= h.Run(map, *rec, (int)index);

				if (h.interrupt != Interrupt::None) break;
			}

			if (rec->opcode == OP_DELAY_TIMER) break;     // always ends the chain
			if (rec->linkIndex == 0) break;
			index = rec->linkIndex;
		}

		if (h.interrupt == Interrupt::Rerun && savedNext != 0)
		{
			h.interrupt = Interrupt::None;
			index = savedNext;
			savedNext = 0;
			continue;                                     // re-run from there
		}
		break;
	}

	return result;
}

} // namespace roth
