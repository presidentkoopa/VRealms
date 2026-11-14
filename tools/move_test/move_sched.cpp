// move_sched: the engine's schedule (two 70 Hz ticks and one frame of fts 2 a 35 Hz tic,
// roth_playermove_engine.cpp) through the engine-free port, from a map's own start, printing the
// line roth_move_log 1 prints in the engine. What the bridge cfg tools/claude_bridge/cfg/
// move_flat.cfg should log on the owner's PC, tic for tic.
//
//   move_sched MAP.RAW "<from>-<to>:<keys> ..." [tics]
//   from / to: the cfg's roth_at tics of the +key and the -key. roth_at runs its commands at the END
//   of a tic (TickLevelLogic's DrainTimedCommands), after that tic's command was read in
//   P_PlayerThink, so the engine first sees a key at tic from+1 and last at tic to (measured on the
//   owner's PC, 367c710c: +forward at roth_at 20 -> keys=002 from tic 21).
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
// A 16.16 value as a decimal with 4 places, rounded half up by integer arithmetic: printf's own
// rounding of an exact tie differs between C runtimes (glibc 647.1562, MSVC 647.1563).
static const char *Fix4(uint32_t v, char *buf)
{
	const int32_t s = (int32_t)v;
	const uint64_t a = s < 0 ? (uint64_t)(-(int64_t)s) : (uint64_t)s;
	uint64_t ip = a >> 16, fp = ((a & 0xffff) * 10000 + 0x8000) >> 16;
	if (fp >= 10000) { ip++; fp -= 10000; }
	snprintf(buf, 32, "%s%llu.%04llu", s < 0 ? "-" : "", (unsigned long long)ip, (unsigned long long)fp);
	return buf;
}
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
		for (const Span &sp : spans) if (t > sp.a && t <= sp.b) in.held |= sp.keys;
		pm.Tick(in); pm.Tick(in); pm.Frame(2); pm.ClampViewPitch();
		char bx[32], by[32], bz[32];
		printf("tic %d x=%s y=%s z=%s angle=%u sector=%d height=%u eye=%u fall=%d,%d jump=%d crouch=%d keys=%03x\n", t,
			Fix4(pm.x, bx), Fix4(pm.y, by), Fix4(pm.z, bz), pm.angle & 0x1ff, pm.SectorIndex(pm.sector),
			pm.height, pm.camHeight, pm.fallState, pm.fallAccum, pm.jumpPhase, pm.crouchMode, in.held);
	}
	return 0;
}
