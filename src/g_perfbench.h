/*
** g_perfbench.h
**
** RS FORK -- THE BENCH HEADER AND THE PER-TIC STATE HASH IN A DEMO
** ("Engine docs/TELEMETRY_AND_BENCH_PLAN.md" sections 3 and 4, "Engine docs/TELEMETRY_IMPL_NOTES.md").
**
** WHAT THE DEMO ALREADY CARRIES, and why there is no second capture format. A VR demo (p_vrdemo.h) already is
** the capture the plan asks for: `record` writes the usercmd stream as demos always have, and DEM_VRFRAME adds
** the VR pose track -- head and both hands, the between-tic pawn moves, the grip state -- which is the part a
** plain demo was missing. Recording a fight is therefore `record <name>`, and replaying it is `playdemo <name>`.
**
** WHAT THIS ADDS, in the same file, as two more demo commands beside DEM_VRFRAME:
**   DEM_BENCHHEADER  once, at the start of the recording: the engine build, the exe, the whole load order with
**                    each file's size and fingerprint, every cvar that differs from its default, the map, the
**                    skill and the RNG seed -- so two runs can be PROVEN comparable rather than assumed to be.
**   DEM_BENCHHASH    every tic: the playsim's state as eight field-group hashes and their total.
**
** THE DESYNC DETECTOR. Playing the demo back compares each tic's hash against the recording's and reports the
** first divergence once, as `DESYNC at tic N`, naming the field group that moved. This is the netplay guard the
** performance plan's playsim phases (sight caching, AI staggering, data layout, the parallel playsim) need: a
** change that quietly alters the world is caught at tic N instead of in somebody's game. It is also why a
** benchmark run can never be mistaken for a faster build when it is really a different one.
**
** THE HASH IS TAKEN AT THE TOP OF THE TIC -- the state the previous tic produced -- on BOTH sides, because that
** is where G_WriteDemoTiccmd writes and where G_ReadDemoTiccmd reads. The two are symmetric by construction.
**
** IT NEVER WRITES PLAYSIM STATE. Hashing walks the thinker list read-only. No RNG is drawn, nothing is keyed on
** a player, and nothing is sent over the network: both commands are written to demos only and are refused in
** multiplayer, exactly as DEM_VRFRAME is.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include <cstdint>
#include <cstddef>

#include "tarray.h"
#include "zstring.h"

namespace PerfBench
{
	// THE STATE HASH, in field groups, so a divergence names what moved rather than only that something did.
	// The order is the order they are compared in, and the first group that differs is the one reported.
	enum EHashGroup
	{
		HASH_POS = 0,		// every actor's X, Y, Z (raw bits: a rounded compare would hide a real drift)
		HASH_VEL,			// every actor's velocity
		HASH_ANGLES,		// every actor's yaw, pitch and roll, in BAMs
		HASH_HEALTH,		// every actor's health, and the players'
		HASH_STATE,			// sprite, frame, tics and class: where each actor is in its states
		HASH_FLAGS,			// flags, flags2, flags3
		HASH_PLAYER,		// the players' own numbers and the tic's usercmds
		HASH_COUNTERS,		// actor count, maptime, kills/items/secrets, and the named RNGs' seeds
		HASH_GROUPS
	};
	const char* HashGroupName(int group);

	struct TicHash
	{
		uint64_t Total = 0;
		uint32_t Group[HASH_GROUPS] = {};
	};

	// Walks the level READ-ONLY and hashes it. Deterministic: the thinker list's own order, raw field bits, no
	// pointer values and no allocation. Safe with no level loaded (everything stays zero).
	TicHash HashLevel();

	// ---- recording, beside VRDemo_* in g_game.cpp -----------------------------------------------------------
	// Cleared where VRDemo_Reset is: a recording or a playback starting or stopping.
	void Reset();
	// Bytes the next Write will put in the stream; 0 = nothing to write (perf_bench_hash off, or not recording).
	size_t PendingSize();
	// G_WriteDemoTiccmd, after the tic's VR frame: the header on the first tic of the recording, then this tic's
	// hash. Sized by PendingSize, which the caller has already made room for.
	void Write(TArrayView<uint8_t>& stream);

	// ---- playback, from Net_DoCommand (d_net.cpp) -----------------------------------------------------------
	void ReadHeader(TArrayView<uint8_t>& stream);
	void ReadHash(TArrayView<uint8_t>& stream);

	// ---- the end of a playback ------------------------------------------------------------------------------
	// g_game.cpp G_CheckDemoStatus, where a playback finishes, BEFORE Reset clears what the demo carried.
	//
	// THE OWNER WATCHES; THE LANE READS THE SAME RUN. While perf_track is on, ANY playback ends by writing the
	// per-frame CSV and the summary -- a demo the owner is watching in the headset, not only an unattended
	// -benchdemo. One recording therefore serves both: they see the fight, and the numbers from that very
	// viewing are on disk when it ends. It also writes the two run headers side by side (the one the demo was
	// recorded under, and the one it was just played back under) so the two can be proven comparable.
	void EndPlayback();

	// ---- what the run found ---------------------------------------------------------------------------------
	const FString& RecordedHeader();	// the header text the demo carried, empty when it carried none
	bool HaveHashes();					// the demo carried hashes at all
	bool Desynced();
	int DesyncTic();					// the tic the first divergence was seen at, -1 when there was none
	int ComparedTics();
	// One line for a summary or a log: what was compared and what came of it.
	FString Verdict();
}

// RS FORK -- -benchdemo: THE UNATTENDED REPLAY (g_benchdemo.cpp). `playdemo` with the per-frame record
// on, run to the end and then quit, writing the CSV, the summary and the verdict. There is no second
// playback path: the demo system plays the demo and the VR demo frame feeds the hands and the body.
namespace BenchDemo
{
	// d_main.cpp, where -playdemo is handled. `mode` is "locked" (the default) or "unlocked".
	void Begin(const char* demoName, const char* mode);
	// True between Begin and Finish, so the demo's own end can tell a benchmark from a watched replay.
	bool Active();
	// g_game.cpp, where a single demo ends: writes the CSV, the summary and the two headers, then quits.
	void Finish();
}

// RS FORK -- -benchdemo: THE UNATTENDED REPLAY (g_benchdemo.cpp). `playdemo` with the per-frame record
// on, run to the end and then quit, writing the CSV, the summary and the verdict. There is no second
// playback path: the demo system plays the demo and the VR demo frame feeds the hands and the body.
namespace BenchDemo
{
	// d_main.cpp, where -playdemo is handled. `mode` is "locked" (the default) or "unlocked".
	void Begin(const char* demoName, const char* mode);
	// True between Begin and Finish, so the demo's own end can tell a benchmark from a watched replay.
	bool Active();
	// g_game.cpp, where a single demo ends: writes the CSV, the summary and the two headers, then quits.
	void Finish();
}
