#pragma once
//
// Running Realms of the Haunting's level logic.
//
// A level carries small programs: lists of records chained by a 1-based index.
// This file is the SPINE that walks and executes them, transcribed from ROTH.C
// rather than reconstructed -- see the citations on each function in the .cpp.
// It is deliberately engine-free so the standalone self-test can exercise it:
// what a command actually DOES is supplied by the caller through Handlers.
//
// The decoded opcode set and what the arguments point at are in
// ROTH_COMMANDS.md. The reader (roth_raw) has already resolved each command's
// key to the sector or faces it names.
//

#include <stdint.h>
#include <functional>

namespace roth
{

struct Map;
struct Command;

// What a command did, as the original reports it: -1 acted, 0 did nothing.
// The executor ORs these together across a chain, and the low bit drives the
// post-chain sound in the original.
enum { CMD_ACTED = -1, CMD_NOTHING = 0 };

// How a handler can interrupt the chain it is running in. The original keeps
// this in g_command_chain_interrupt (0x8a268).
enum class Interrupt
{
	None = 0,
	Break = 1,      // stop the chain here
	Rerun = 2,      // stop, then re-run from the latched "next active" index
};

struct Handlers
{
	// Runs one command. Return CMD_ACTED or CMD_NOTHING. Anything the caller
	// does not implement should return CMD_NOTHING **and be counted**, so an
	// unhandled opcode is visible rather than silent.
	std::function<int(const Map &, const Command &, int index)> Run;

	// Set by a handler that wants to break or re-run the chain.
	Interrupt interrupt = Interrupt::None;

	// The original's g_command_next_active (0x8a0cc): opcode 0x38 latches an
	// index here, and the executor re-runs from it when a chain ends with
	// Interrupt::Rerun.
	uint16_t nextActive = 0;
};

// The FLOW pre-pass. Follows the chain's NEXT links running ONLY the two gate
// opcodes -- it never touches an action handler. Returns 0 when the chain is
// clear to run, or the 1-based index of the record that is holding it up.
uint16_t WalkChainFlow(const Map &map, uint16_t index);

// The ACTION pass. Walks the chain from a 1-based index, dispatching each
// record that is not disabled. Returns the accumulated handler results.
int ExecChain(const Map &map, uint16_t index, Handlers &h);

} // namespace roth
