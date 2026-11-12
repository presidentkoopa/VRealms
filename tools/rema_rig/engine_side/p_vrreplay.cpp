/*
** p_vrreplay.cpp
**
** [WEAPONWORLDAUDIT] A file-driven hand input source. See p_vrreplay.h.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

#include "p_vrreplay.h"
#include "d_protocol.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "doomstat.h"
#include "printf.h"
#include "zstring.h"

CVAR(String, vr_replay_play, "", 0)
CVAR(String, vr_replay_record, "", 0)
CVAR(Bool, vr_replay_loop, false, 0)

namespace
{
	// One row of the file, already in wire units. 'buttons' is OR'd into the command.
	struct ReplayRow
	{
		int rep = 1;
		vrhandpose_t mainhand{};
		vrhandpose_t offhand{};
		vrheadpose_t head{};
		vrhandinput_t handinput{};
		uint32_t buttons = 0;
	};

	// The number of integer columns after 'rep'. Fixed: a row with a different count is an
	// error, not a guess, because a shifted column puts a hand somewhere nobody asked for.
	constexpr int kColumns = 6 + 6 + 3 + 10 + 1;

	struct ReplayState
	{
		FString path;				// what vr_replay_play held when we loaded
		std::vector<ReplayRow> rows;
		size_t row = 0;				// index into rows
		int repLeft = 0;			// tics still to repeat rows[row]
		int applied = 0;			// 1-based row number last applied
		bool active = false;
	};
	ReplayState gPlay;

	struct RecordState
	{
		FString path;
		FILE* f = nullptr;
	};
	RecordState gRec;

	bool ParseRow(const char* line, ReplayRow& out, int lineNo, const char* path)
	{
		// Strip comment, skip blanks.
		std::string s(line);
		size_t hash = s.find('#');
		if (hash != std::string::npos) s.erase(hash);
		std::vector<long> v;
		const char* p = s.c_str();
		char* end = nullptr;
		for (;;)
		{
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
			if (*p == 0) break;
			long val = strtol(p, &end, 10);
			if (end == p)
			{
				Printf(TEXTCOLOR_RED "vr_replay: %s:%d: not a number at '%.8s'\n", path, lineNo, p);
				return false;
			}
			v.push_back(val);
			p = end;
		}
		if (v.empty()) return false;	// blank or comment-only: caller treats as skip
		if ((int)v.size() != 1 + kColumns)
		{
			Printf(TEXTCOLOR_RED "vr_replay: %s:%d: %d columns, expected %d (rep + %d values)\n",
				path, lineNo, (int)v.size(), 1 + kColumns, kColumns);
			return false;
		}
		size_t i = 0;
		out.rep = (int)v[i++];
		if (out.rep < 1) out.rep = 1;
		for (int k = 0; k < 3; ++k) out.mainhand.pos[k] = (short)v[i++];
		for (int k = 0; k < 3; ++k) out.mainhand.ang[k] = (short)v[i++];
		for (int k = 0; k < 3; ++k) out.offhand.pos[k] = (short)v[i++];
		for (int k = 0; k < 3; ++k) out.offhand.ang[k] = (short)v[i++];
		out.head.z = (short)v[i++];
		out.head.pitch = (short)v[i++];
		out.head.roll = (short)v[i++];
		for (int k = 0; k < 2; ++k) out.handinput.grip[k] = (uint8_t)v[i++];
		for (int k = 0; k < 2; ++k) out.handinput.trigger[k] = (uint8_t)v[i++];
		for (int k = 0; k < 2; ++k) out.handinput.handbits[k] = (uint8_t)v[i++];
		for (int k = 0; k < 4; ++k) out.handinput.thumb[k] = (int8_t)v[i++];
		out.buttons = (uint32_t)v[i++];
		return true;
	}

	bool LoadFile(const char* path)
	{
		gPlay = ReplayState();
		FILE* f = fopen(path, "r");
		if (f == nullptr)
		{
			Printf(TEXTCOLOR_RED "vr_replay: cannot open '%s'\n", path);
			return false;
		}
		char line[1024];
		int lineNo = 0;
		bool sawHeader = false;
		int totalTics = 0;
		while (fgets(line, sizeof line, f))
		{
			++lineNo;
			if (!sawHeader)
			{
				if (strncmp(line, "# vrreplay 1", 12) != 0)
				{
					Printf(TEXTCOLOR_RED "vr_replay: %s: first line must be '# vrreplay 1'\n", path);
					fclose(f);
					return false;
				}
				sawHeader = true;
				continue;
			}
			ReplayRow r;
			// A parse error aborts the whole load: a half-loaded replay is a wrong replay.
			std::string s(line);
			size_t hash = s.find('#');
			if (hash != std::string::npos) s.erase(hash);
			bool blank = s.find_first_not_of(" \t\r\n") == std::string::npos;
			if (blank) continue;
			if (!ParseRow(line, r, lineNo, path))
			{
				fclose(f);
				gPlay = ReplayState();
				return false;
			}
			totalTics += r.rep;
			gPlay.rows.push_back(r);
		}
		fclose(f);
		if (gPlay.rows.empty())
		{
			Printf(TEXTCOLOR_RED "vr_replay: %s has no rows\n", path);
			return false;
		}
		gPlay.path = path;
		gPlay.active = true;
		gPlay.row = 0;
		gPlay.repLeft = gPlay.rows[0].rep;
		Printf("vr_replay: playing %s: %d rows, %d tics%s\n", path, (int)gPlay.rows.size(), totalTics,
			vr_replay_loop ? ", looping" : "");
		return true;
	}

	void StopPlay(const char* why)
	{
		if (gPlay.active) Printf("vr_replay: stopped (%s)\n", why);
		gPlay = ReplayState();
		vr_replay_play = "";
	}

	void Apply(const ReplayRow& r, usercmd_t* cmd)
	{
		cmd->mainhand = r.mainhand;
		cmd->offhand = r.offhand;
		// The replay records z/pitch/roll only; the room travel and the turn are the
		// command's own (a replay file never carried them) and are kept.
		cmd->head.z = r.head.z;
		cmd->head.pitch = r.head.pitch;
		cmd->head.roll = r.head.roll;
		cmd->handinput = r.handinput;
		cmd->buttons |= r.buttons;
	}

	void WriteRow(FILE* f, const usercmd_t* cmd)
	{
		fprintf(f, "1  %d %d %d %d %d %d   %d %d %d %d %d %d   %d %d %d   %u %u %u %u %u %u %d %d %d %d   %u\n",
			cmd->mainhand.pos[0], cmd->mainhand.pos[1], cmd->mainhand.pos[2],
			cmd->mainhand.ang[0], cmd->mainhand.ang[1], cmd->mainhand.ang[2],
			cmd->offhand.pos[0], cmd->offhand.pos[1], cmd->offhand.pos[2],
			cmd->offhand.ang[0], cmd->offhand.ang[1], cmd->offhand.ang[2],
			cmd->head.z, cmd->head.pitch, cmd->head.roll,
			cmd->handinput.grip[0], cmd->handinput.grip[1],
			cmd->handinput.trigger[0], cmd->handinput.trigger[1],
			cmd->handinput.handbits[0], cmd->handinput.handbits[1],
			cmd->handinput.thumb[0], cmd->handinput.thumb[1], cmd->handinput.thumb[2], cmd->handinput.thumb[3],
			(unsigned)cmd->buttons);
	}
}

bool VRReplay_Playing() { return gPlay.active; }
int VRReplay_Row() { return gPlay.active ? gPlay.applied : 0; }

// [NETPLAY] True while a replay file is driving this machine's hands. G_BuildTiccmd folds it
// into vr_handaim, so a replayed player is a hand-aimed player to every machine, and the
// netplay rig exercises the VR path with no headset.
bool VRReplay_IsPlaying()
{
	const char* want = vr_replay_play;
	return want != nullptr && want[0] != 0;
}

void VRReplay_OnBuildTiccmd(usercmd_t* cmd)
{
	if (cmd == nullptr) return;

	// ---- playback -------------------------------------------------------
	const char* want = vr_replay_play;
	if (want != nullptr && want[0] != 0)
	{
		if (!gPlay.active || gPlay.path.Compare(want) != 0)
		{
			if (!LoadFile(want))
			{
				vr_replay_play = "";
			}
		}
	}
	else if (gPlay.active)
	{
		StopPlay("vr_replay_play cleared");
	}

	if (gPlay.active)
	{
		const ReplayRow& r = gPlay.rows[gPlay.row];
		Apply(r, cmd);
		gPlay.applied = (int)gPlay.row + 1;
		if (--gPlay.repLeft <= 0)
		{
			if (++gPlay.row >= gPlay.rows.size())
			{
				if (vr_replay_loop)
				{
					gPlay.row = 0;
				}
				else
				{
					StopPlay("end of file");
				}
			}
			if (gPlay.active) gPlay.repLeft = gPlay.rows[gPlay.row].rep;
		}
	}

	// ---- recording ------------------------------------------------------
	const char* rec = vr_replay_record;
	const bool wantRec = rec != nullptr && rec[0] != 0;
	if (wantRec && (gRec.f == nullptr || gRec.path.Compare(rec) != 0))
	{
		if (gRec.f) fclose(gRec.f);
		gRec.f = fopen(rec, "a");
		if (gRec.f == nullptr)
		{
			Printf(TEXTCOLOR_RED "vr_replay: cannot open '%s' for recording\n", rec);
			vr_replay_record = "";
		}
		else
		{
			gRec.path = rec;
			fprintf(gRec.f, "# vrreplay 1\n# recorded by vr_replay_record\n");
			Printf("vr_replay: recording to %s\n", rec);
		}
	}
	else if (!wantRec && gRec.f != nullptr)
	{
		fclose(gRec.f);
		gRec = RecordState();
		Printf("vr_replay: recording stopped\n");
	}
	if (gRec.f != nullptr)
	{
		WriteRow(gRec.f, cmd);
		fflush(gRec.f);
	}
}
