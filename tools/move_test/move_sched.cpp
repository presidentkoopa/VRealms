// move_sched: the engine's schedule (two 70 Hz ticks and one frame of fts 2 a 35 Hz tic,
// roth_playermove_engine.cpp) through the engine-free port, from a map's own start, printing the
// line roth_move_log 1 prints in the engine. What the bridge cfg tools/claude_bridge/cfg/
// move_flat.cfg should log on the owner's PC, tic for tic.
//
//   move_sched MAP.RAW "<from>-<to>:<keys> ..." [tics]
//   keys: f(orward) b(ack) l(eft turn) r(ight turn) s(trafe left) d (strafe right) j(ump) c(rouch)
//   MOVE_SCHED_RUN=1 in the environment: the run toggle on (the engine's cl_run)
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "roth_playermove.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sstream>
using namespace roth;
struct NoHost : PlayerMoveHost { void DamageDirect(uint32_t) override {} };
int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: move_sched MAP.RAW \"from-to:keys ...\" [tics]\n"); return 2; }
	FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
	std::vector<uint8_t> raw; { fseek(f, 0, SEEK_END); raw.resize(ftell(f)); fseek(f, 0, SEEK_SET); if (fread(raw.data(), 1, raw.size(), f) != raw.size()) return 2; fclose(f); }
	PlayerMove pm; NoHost host; pm.host = &host;
	if (!pm.BuildImage(raw.data(), raw.size())) return 1;
	pm.InitTuning();
	// init_player_position_from_metadata (renderer.c:10285): x, z, y, angle; the sector by the search
	const uint8_t *m = raw.data() + (raw[0xa] | (raw[0xb] << 8));
	auto S = [&](int o) { return (int16_t)(m[o] | (m[o + 1] << 8)); };
	pm.x = (uint32_t)(uint16_t)S(0) << 16; pm.z = (uint32_t)(uint16_t)S(2) << 16; pm.y = (uint32_t)(uint16_t)S(4) << 16; pm.angle = (uint16_t)S(6);
	pm.sector = pm.Locate(S(0), S(4), S(2), 0);
	struct Span { int a, b; uint32_t keys; };
	std::vector<Span> spans;
	{
		std::istringstream is(argv[2]); std::string t;
		while (is >> t)
		{
			Span sp{}; char k[32] = {};
			if (sscanf(t.c_str(), "%d-%d:%31s", &sp.a, &sp.b, k) != 3) continue;
			for (char *p = k; *p; p++) switch (*p)
			{
			case 'f': sp.keys |= 1u << MK_FORWARD; break; case 'b': sp.keys |= 1u << MK_BACK; break;
			case 'l': sp.keys |= 1u << MK_TURN_LEFT; break; case 'r': sp.keys |= 1u << MK_TURN_RIGHT; break;
			case 's': sp.keys |= 1u << MK_STRAFE_LEFT; break; case 'd': sp.keys |= 1u << MK_STRAFE_RIGHT; break;
			case 'j': sp.keys |= 1u << MK_JUMP; break; case 'c': sp.keys |= 1u << MK_CROUCH; break;
			}
			spans.push_back(sp);
		}
	}
	const int tics = argc > 3 ? atoi(argv[3]) : 200;
	if (getenv("MOVE_SCHED_RUN") && atoi(getenv("MOVE_SCHED_RUN"))) pm.runToggle = 1;
	for (int t = 0; t < tics; t++)
	{
		MoveInput in;
		for (const Span &sp : spans) if (t >= sp.a && t < sp.b) in.held |= sp.keys;
		pm.Tick(in); pm.Tick(in); pm.Frame(2); pm.ClampViewPitch();
		printf("tic %d x=%.4f y=%.4f z=%.4f angle=%u sector=%d height=%u eye=%u fall=%d,%d jump=%d crouch=%d keys=%03x\n", t,
			(int32_t)pm.x / 65536., (int32_t)pm.y / 65536., (int32_t)pm.z / 65536., pm.angle & 0x1ff, pm.SectorIndex(pm.sector),
			pm.height, pm.camHeight, pm.fallState, pm.fallAccum, pm.jumpPhase, pm.crouchMode, in.held);
	}
	return 0;
}
