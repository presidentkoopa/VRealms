/*
** g_perfbench.cpp
**
** RS FORK -- the bench header and the per-tic state hash in a demo. See g_perfbench.h.
**
** THE TWO PAYLOADS, both "Word: size, then the payload", exactly as DEM_VRFRAME is framed:
**
**   DEM_BENCHHEADER   the header text, "# key=value" lines. Written once, on the first recorded tic.
**   DEM_BENCHHASH     u64 total, then HASH_GROUPS x u32, little endian, written field by field so the format
**                     does not depend on a struct's padding.
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

#include "g_perfbench.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "actor.h"
#include "c_cvars.h"
#include "d_net.h"
#include "d_player.h"
#include "d_protocol.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "m_random.h"
#include "printf.h"
#include "hwrenderer/data/hw_perftrack.h"	// RS FORK -- perf_track: AppendRunHeader, shared so one run has one header

EXTERN_CVAR(Int, gameskill)
EXTERN_CVAR(Int, perf_track)

extern FString defdemoname;		// g_game.cpp: the demo this playback is of

// The four RNGs the network consistency check already trusts to stand for the playsim's draw order (d_net.cpp).
// Nothing here draws from them; only their seeds are read.
extern FRandom pr_spawnmobj;
extern FRandom pr_acs;
extern FRandom pr_chase;
extern FRandom pr_damagemobj;

// RS FORK -- perf_bench_hash: while a demo is being recorded, each tic's state hash goes into it, and while one
// is played back the hashes are compared. On by default, because a capture without hashes cannot be trusted as a
// benchmark: a build that changed the world would look like a build that got faster. Costs one read-only walk of
// the thinker list a tic, and only ever while a demo is recording.
CVARD(Bool, perf_bench_hash, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "record a per-tic playsim state hash into demos, and compare it on playback (the DESYNC detector)")

namespace
{
	// The recording side.
	bool HeaderWritten = false;

	// The playback side.
	FString Header;
	bool AnyHashes = false;
	bool IsDesynced = false;
	int FirstDesyncTic = -1;
	int DesyncGroup = -1;
	uint32_t DesyncWanted = 0, DesyncGot = 0;
	int Compared = 0;

	inline void Mix(uint64_t& h, uint64_t v)
	{
		for (int i = 0; i < 8; i++)
		{
			h ^= (uint8_t)((v >> (i * 8)) & 0xff);
			h *= 0x100000001b3ull;
		}
	}
	inline void MixD(uint64_t& h, double v)
	{
		uint64_t bits;
		memcpy(&bits, &v, sizeof(bits));
		Mix(h, bits);
	}

	FString Stamp()
	{
		time_t now = time(nullptr);
		struct tm t;
#ifdef _WIN32
		localtime_s(&t, &now);
#else
		localtime_r(&now, &t);
#endif
		FString s;
		s.Format("%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
		return s;
	}

	// The header text, built once when a recording starts. The load order, the exe and the changed cvars come
	// from perf_track's own AppendRunHeader, so a capture's header and a measured run's header are the same
	// text and bench_compare.py has one format to read.
	FString BuildHeader()
	{
		FString out;
		PerfTrack::AppendRunHeader(out);
		out.AppendFormat("# map=%s\n", primaryLevel != nullptr ? primaryLevel->MapName.GetChars() : "");
		out.AppendFormat("# skill=%d\n", (int)*gameskill);
		out.AppendFormat("# rngseed=%u\n", (unsigned)rngseed);
		out.AppendFormat("# hash_groups=%d\n", (int)PerfBench::HASH_GROUPS);
		// The demo's length prefix is a word. A load order long enough to overflow it is not a load order we can
		// describe honestly, so say so rather than truncate silently in the middle of a line.
		if (out.Len() > 60000)
		{
			out.Truncate(59900);
			out << "# TRUNCATED: the header did not fit one demo command\n";
		}
		return out;
	}
}

const char* PerfBench::HashGroupName(int group)
{
	static const char* const names[HASH_GROUPS] =
		{ "position", "velocity", "angles", "health", "state", "flags", "player", "counters" };
	return (group >= 0 && group < HASH_GROUPS) ? names[group] : "?";
}

//==========================================================================
//
// HashLevel
//
// One tic of the playsim, read-only, as eight field-group hashes and their
// total. FNV-1a over RAW BITS: a rounded or printed compare would hide the
// slow drift a reordered float sum causes, which is exactly the failure the
// performance plan's playsim phases risk.
//
//==========================================================================

PerfBench::TicHash PerfBench::HashLevel()
{
	TicHash out;
	uint64_t g[HASH_GROUPS];
	for (int i = 0; i < HASH_GROUPS; i++)
		g[i] = 0xcbf29ce484222325ull;

	FLevelLocals* level = primaryLevel;
	uint64_t actors = 0;
	if (level != nullptr)
	{
		// The thinker list's own order -- the order the playsim ticks in. Keeping the two in step is the whole
		// reason this hash exists, so it must never be sorted or reordered here.
		TThinkerIterator<AActor> it(level);
		AActor* mo;
		while ((mo = it.Next()) != nullptr)
		{
			actors++;
			MixD(g[HASH_POS], mo->X()); MixD(g[HASH_POS], mo->Y()); MixD(g[HASH_POS], mo->Z());
			MixD(g[HASH_VEL], mo->Vel.X); MixD(g[HASH_VEL], mo->Vel.Y); MixD(g[HASH_VEL], mo->Vel.Z);
			Mix(g[HASH_ANGLES], (uint64_t)(uint32_t)mo->Angles.Yaw.BAMs());
			Mix(g[HASH_ANGLES], (uint64_t)(uint32_t)mo->Angles.Pitch.BAMs());
			Mix(g[HASH_ANGLES], (uint64_t)(uint32_t)mo->Angles.Roll.BAMs());
			Mix(g[HASH_HEALTH], (uint64_t)(int64_t)mo->health);
			Mix(g[HASH_STATE], (uint64_t)(uint32_t)mo->sprite);
			Mix(g[HASH_STATE], (uint64_t)(uint32_t)mo->frame);
			Mix(g[HASH_STATE], (uint64_t)(int64_t)mo->tics);
			Mix(g[HASH_STATE], (uint64_t)(int64_t)mo->GetClass()->TypeName.GetIndex());
			Mix(g[HASH_FLAGS], (uint64_t)mo->flags.GetValue());
			Mix(g[HASH_FLAGS], (uint64_t)mo->flags2.GetValue());
			Mix(g[HASH_FLAGS], (uint64_t)mo->flags3.GetValue());
		}
		Mix(g[HASH_COUNTERS], (uint64_t)(int64_t)level->maptime);
		Mix(g[HASH_COUNTERS], (uint64_t)(int64_t)level->killed_monsters);
		Mix(g[HASH_COUNTERS], (uint64_t)(int64_t)level->found_items);
		Mix(g[HASH_COUNTERS], (uint64_t)(int64_t)level->found_secrets);
	}
	Mix(g[HASH_COUNTERS], actors);
	Mix(g[HASH_COUNTERS], (uint64_t)(uint32_t)pr_spawnmobj.Seed());
	Mix(g[HASH_COUNTERS], (uint64_t)(uint32_t)pr_acs.Seed());
	Mix(g[HASH_COUNTERS], (uint64_t)(uint32_t)pr_chase.Seed());
	Mix(g[HASH_COUNTERS], (uint64_t)(uint32_t)pr_damagemobj.Seed());

	for (int i = 0; i < MAXPLAYERS; i++)
	{
		if (!playeringame[i])
			continue;
		Mix(g[HASH_PLAYER], (uint64_t)(int64_t)i);
		Mix(g[HASH_PLAYER], (uint64_t)(int64_t)players[i].health);
		Mix(g[HASH_PLAYER], (uint64_t)(int64_t)players[i].playerstate);
		Mix(g[HASH_PLAYER], (uint64_t)players[i].cmd.buttons);
		Mix(g[HASH_PLAYER], (uint64_t)(uint16_t)players[i].cmd.pitch);
		Mix(g[HASH_PLAYER], (uint64_t)(uint16_t)players[i].cmd.yaw);
		Mix(g[HASH_PLAYER], (uint64_t)(uint16_t)players[i].cmd.forwardmove);
		Mix(g[HASH_PLAYER], (uint64_t)(uint16_t)players[i].cmd.sidemove);
		Mix(g[HASH_HEALTH], (uint64_t)(int64_t)players[i].health);
	}

	uint64_t total = 0xcbf29ce484222325ull;
	for (int i = 0; i < HASH_GROUPS; i++)
	{
		out.Group[i] = (uint32_t)(g[i] ^ (g[i] >> 32));
		Mix(total, g[i]);
	}
	out.Total = total;
	return out;
}

//==========================================================================
//
// Recording
//
//==========================================================================

void PerfBench::Reset()
{
	HeaderWritten = false;
	Header = "";
	AnyHashes = false;
	IsDesynced = false;
	FirstDesyncTic = -1;
	DesyncGroup = -1;
	DesyncWanted = DesyncGot = 0;
	Compared = 0;
}

size_t PerfBench::PendingSize()
{
	// Demo-only and single-player-only, exactly as the VR frame is: a multiplayer game never writes these and
	// never reads them, so netplay is untouched.
	if (!demorecording || multiplayer || !perf_bench_hash)
		return 0;
	size_t bytes = 1 + 2 + (size_t)(8 + 4 * HASH_GROUPS);		// DEM_BENCHHASH
	if (!HeaderWritten)
		bytes += 1 + 2 + BuildHeader().Len();					// DEM_BENCHHEADER, once
	return bytes;
}

void PerfBench::Write(TArrayView<uint8_t>& stream)
{
	if (PendingSize() == 0)
		return;

	if (!HeaderWritten)
	{
		HeaderWritten = true;
		const FString header = BuildHeader();
		WriteInt8(DEM_BENCHHEADER, stream);
		WriteInt16((int16_t)(uint16_t)header.Len(), stream);
		WriteBytes(TArrayView<uint8_t>((uint8_t*)header.GetChars(), header.Len()), stream);
	}

	const TicHash h = HashLevel();
	WriteInt8(DEM_BENCHHASH, stream);
	WriteInt16((int16_t)(uint16_t)(8 + 4 * HASH_GROUPS), stream);
	for (int i = 0; i < 8; i++)
		WriteInt8((uint8_t)((h.Total >> (i * 8)) & 0xff), stream);
	for (int gi = 0; gi < HASH_GROUPS; gi++)
		for (int i = 0; i < 4; i++)
			WriteInt8((uint8_t)((h.Group[gi] >> (i * 8)) & 0xff), stream);
}

//==========================================================================
//
// Playback
//
//==========================================================================

void PerfBench::ReadHeader(TArrayView<uint8_t>& stream)
{
	const size_t len = (uint16_t)ReadInt16(stream);
	if (stream.Size() < len)
	{
		AdvanceStream(stream, stream.Size());
		return;
	}
	Header = FString((const char*)stream.Data(), len);
	AdvanceStream(stream, len);
	Printf("bench: this demo carries a run header (%u bytes). Compare it with bench_compare.py.\n", (unsigned)len);
}

void PerfBench::ReadHash(TArrayView<uint8_t>& stream)
{
	const size_t len = (uint16_t)ReadInt16(stream);
	const size_t want = (size_t)(8 + 4 * HASH_GROUPS);
	if (stream.Size() < len)
	{
		AdvanceStream(stream, stream.Size());
		return;
	}
	if (len != want)
	{
		// A demo whose hash record is a different shape was made by a different build of this file. Skip it
		// rather than compare nonsense and cry desync at every tic.
		AdvanceStream(stream, len);
		if (!AnyHashes)
		{
			AnyHashes = true;
			Printf(TEXTCOLOR_ORANGE "bench: this demo's state hashes are %u bytes, this build reads %u -- not compared.\n",
				(unsigned)len, (unsigned)want);
		}
		return;
	}

	TicHash wanted;
	const uint8_t* p = stream.Data();
	for (int i = 0; i < 8; i++)
		wanted.Total |= (uint64_t)p[i] << (i * 8);
	for (int gi = 0; gi < HASH_GROUPS; gi++)
		for (int i = 0; i < 4; i++)
			wanted.Group[gi] |= (uint32_t)p[8 + gi * 4 + i] << (i * 8);
	AdvanceStream(stream, len);

	// A recording never reads its own stream.
	if (!demoplayback)
		return;
	AnyHashes = true;
	Compared++;

	const TicHash got = HashLevel();
	if (got.Total == wanted.Total || IsDesynced)
		return;		// reported once: after the first divergence every later tic differs for the same reason

	IsDesynced = true;
	FirstDesyncTic = Compared;
	for (int i = 0; i < HASH_GROUPS; i++)
	{
		if (got.Group[i] != wanted.Group[i])
		{
			DesyncGroup = i;
			DesyncWanted = wanted.Group[i];
			DesyncGot = got.Group[i];
			break;
		}
	}
	Printf(TEXTCOLOR_RED "DESYNC at tic %d: first differing field group = %s (expected %08x, got %08x)\n",
		FirstDesyncTic, HashGroupName(DesyncGroup), DesyncWanted, DesyncGot);
	Printf(TEXTCOLOR_RED "DESYNC: the playsim did something different from the recording. This run's timings are "
		"NOT comparable with the recording's build.\n");
}

//==========================================================================
//
// EndPlayback
//
// A playback has finished. While perf_track is on this is where the run is
// written out -- for a demo the owner is WATCHING exactly as much as for an
// unattended -benchdemo, so one recording serves the viewing and the reading
// of it. Called before Reset, which clears what the demo carried.
//
//==========================================================================

void PerfBench::EndPlayback()
{
	if (!demoplayback)
		return;

	FString verdict;
	verdict.Format("playback demo=%s %s", defdemoname.IsEmpty() ? "(unnamed)" : defdemoname.GetChars(),
		Verdict().GetChars());

	// The two headers side by side: what the demo was recorded under, and what it was just played back under.
	// Only worth a file when there is something in it -- a demo with no bench header watched with the record
	// off writes nothing at all.
	if (*perf_track > 0 || !Header.IsEmpty())
	{
		const FString path = PerfTrack::OutputDir() + "benchrun_" + Stamp() + ".txt";
		FString out;
		out.AppendFormat("%s\n\n", verdict.GetChars());
		out << "## the header this demo was RECORDED under\n";
		if (Header.IsEmpty())
			out << "# (none: recorded by a build without the bench header, or with perf_bench_hash off)\n";
		else
			out << Header;
		out << "\n## the header this run PLAYED IT BACK under\n";
		PerfTrack::AppendRunHeader(out);
		FILE* f = fopen(path.GetChars(), "wt");
		if (f != nullptr)
		{
			fputs(out.GetChars(), f);
			fclose(f);
			Printf("bench: run headers written to %s\n", path.GetChars());
		}
	}

	// The per-frame CSV and the summary, with the verdict on the summary's first line. A no-op when
	// perf_track never recorded anything.
	if (*perf_track > 0)
		PerfTrack::Flush(verdict.GetChars());

	Printf("%s\n", verdict.GetChars());
}

const FString& PerfBench::RecordedHeader() { return Header; }
bool PerfBench::HaveHashes() { return AnyHashes; }
bool PerfBench::Desynced() { return IsDesynced; }
int PerfBench::DesyncTic() { return FirstDesyncTic; }
int PerfBench::ComparedTics() { return Compared; }

FString PerfBench::Verdict()
{
	FString out;
	if (!AnyHashes)
	{
		out = "hashes=none (the demo carries no state hashes: record it with perf_bench_hash on)";
		return out;
	}
	out.Format("hashes=%d result=%s", Compared, IsDesynced ? "DESYNC" : "identical");
	if (IsDesynced)
		out.AppendFormat(" desync_tic=%d desync_group=%s desync_expected=%08x desync_got=%08x",
			FirstDesyncTic, HashGroupName(DesyncGroup), DesyncWanted, DesyncGot);
	return out;
}
