/*
** roth_diff.cpp
** REMAROTH's half of rothdiff: a per-pixel identity buffer for one camera pose.
**
** WHY THIS IS A RAYCAST AND NOT A SCREENSHOT
**
** The question rothdiff answers is "at this pixel, which surface does each
** engine think it is drawing, out of which texture". A screenshot cannot answer
** that -- it only carries colour, so a wrong texture, a wrong scale and a wrong
** light all look the same. Every texture decision on this port that was settled
** from screenshots has had to be reversed at least once.
**
** So instead of reading pixels back off the GPU, this walks the same geometry
** the renderer walks, one ray per pixel, using the engine's OWN trace. What it
** reports is what the loader actually built: which sector or line is there, and
** which texture the loader hung on it. That is precisely the thing under test.
**
** WHAT IT THEREFORE DOES NOT TEST. Anything that happens after surface
** selection: lighting, fog, sprite sorting, translucency. Those need their own
** comparison and must not be assumed covered by a high match percentage here.
**
** The output format is the same .ridb the ROTH.C side writes, so tools/rothdiff
** compares them directly. See tools/rothdiff/rothdiff.py for the layout.
**
**     rothdiff_dump <x> <y> <angle512> <width> <height> <file>
**
** `angle512` is ROTH's angle unit -- 512 per turn -- so a pose can be pasted
** from the ROTH.C side unchanged rather than converted by hand at each call
** site, which is where sign errors live.
*/

#include "c_dispatch.h"
#include "c_cvars.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "p_trace.h"
#include "actor.h"
#include "texturemanager.h"
#include "printf.h"
#include "m_misc.h"
#include "cmdlib.h"
#include "vectors.h"
#include "gametexture.h"
#include "r_data/sprites.h"
#include "p_local.h"   // P_UseLines, for rothdiff_use
#include "roth_runtime.h"   // SidesWithOpcode, to aim a shot at a trigger

#include <stdio.h>
#include <stdint.h>
#include <vector>

namespace
{

// Must match tools/rothdiff/rothdiff.py and tools/rothdiff/idbuffer.inc.c.
const uint32_t RIDB_MAGIC = 0x42444952u;   // 'RIDB'
const uint32_t RIDB_VERSION = 1u;

struct IdPixel
{
	uint16_t fill;
	uint16_t flags;
	uint16_t id;
	uint16_t tex;
};

// ROTH's span fill word, reproduced so both engines land in one taxonomy.
// These are the ORIGINAL's literals, from the visible pass:
//     ceiling textured   0x38   renderer.c:9229
//     ceiling solid      0x30   renderer.c:9223
//     floor   textured   0xb8   renderer.c:9249
//     floor   solid      0xb0   renderer.c:9243
// and g_world_surface_draw_flags bit 0x20 is what routes a span into the
// flat/sprite driver at renderer.c:13314.
const uint16_t FILL_CEIL_TEX = 0x38, FILL_CEIL_SOLID = 0x30;
const uint16_t FILL_FLOOR_TEX = 0xb8, FILL_FLOOR_SOLID = 0xb0;
const uint16_t FLAGS_SPRITE_PATH = 0x0020;

// ROTH's horizontal field of view. focal = view width * 0x7c/256, so
// tan(hfov/2) = (w/2) / (w * 124/256) = 128/124. This is 91.8 degrees, against
// the 90 the engine would otherwise use -- and the difference is exactly the
// kind of thing that reads as "the projection is wrong" when it is only the FOV.
const double ROTH_FOCAL_NUM = 124.0, ROTH_FOCAL_DEN = 256.0;

// ROTH angle units: 512 per full turn, 0 along +Y. This converts a VIEWER
// angle, and Realms measures the viewer COUNTER-CLOCKWISE from +Y -- the same
// convention rothmap.cpp:2044 uses for the player start, and the one the pose
// files carry, because the ROTH.C plugin writes their angle straight into the
// original's own player angle.
//
// OBJECTS use the opposite (clockwise) sense -- ROTH_SURFACES_FIX.md 3.4,
// renderer.c:6093-6126. This function used to apply that object rule to the
// camera, so a capture pair only showed the same place at angle 0 and 256,
// where the two senses coincide. At 128 the two engines looked opposite ways:
// pose A agreed, poses B and C did not. Do not "simplify" the sign back.
inline DAngle RothAngleToDoom(int a512)
{
	return DAngle::fromDeg(90.0 + (double)a512 * (360.0 / 512.0));
}

bool WriteRidb(const char *path, int w, int h, int px, int py, int pang,
	const std::vector<IdPixel> &buf)
{
	FILE *f = fopen(path, "wb");
	if (f == nullptr) return false;
	uint32_t hdr[4] = { RIDB_MAGIC, RIDB_VERSION, (uint32_t)w, (uint32_t)h };
	int32_t pose[3] = { px, py, pang };
	fwrite(hdr, sizeof(uint32_t), 4, f);
	fwrite(pose, sizeof(int32_t), 3, f);
	fwrite(buf.data(), sizeof(IdPixel), buf.size(), f);
	fclose(f);
	return true;
}

} // namespace

namespace
{
// Commands given with +exec on the command line all run BEFORE the first level
// exists -- `map` is deferred, `exec` is not -- so a dump issued that way would
// find gamestate != GS_LEVEL and do nothing. Rather than make every caller
// stage its own delay (and there is no `wait` command in this build), a dump
// asked for too early is QUEUED and drained once a level is up.
struct PendingDump
{
	int x, y, ang, w, h;
	FString path;
	bool screenshot;   // a picture for the eye, not an identity buffer
	int settle;        // frames to let the view settle before shooting
	int pitch = 0;     // ROTH pitch units, screenshots only
};
TArray<PendingDump> g_pending;

// A queued request that is a USE, not a capture. `w` is the width of an identity
// buffer, -1 already means "a sprite listing", so -2 means "press use here".
static const int USE_MARKER = -2;

// -3 means "stand in this SECTOR", with the sector number in `x`.
//
// Needed because a sector is not addressable any other way. The load report
// names the sectors carrying a given trigger opcode -- 23 of them carry an 0x13
// in STUDY1 -- but nothing offline turns a sector number into a point inside
// it, and the rig only takes x,y. findspots finds places by SURFACE TYPE, not
// by sector, so it cannot answer "put me in sector 174".
//
// It waits a few tics after arriving rather than moving straight on, because
// the thing being tested fires from P_PlayerThink on a LATER tic than the move:
// a queue that advanced immediately would quit before the last sector's trigger
// had any tic to run in.
static const int SECTOR_MARKER = -3;
static const int SECTOR_SETTLE_TICS = 4;

// -4 means "stand in this sector, look at the FLOOR, and press use", with the
// sector number in `x`.
//
// A floor-click trigger (0x19) needs the aim pointed DOWN, and nothing else in
// this rig can do that: rothdiff_use is a yaw and a position, and the engine's
// own use path has no pitch in it at all. -126 is the original's full-down view
// pitch, a real pitch of atan(126/128) or about 44 degrees, which from eye
// height reaches the floor well inside the 64-unit use range.
static const int SECTOR_USE_MARKER = -4;
static const int FLOOR_LOOK_PITCH = -126;

// -5 means "shoot this SIDEDEF", with the sidedef index in `x`.
//
// 0x1a is the attack-hits-a-wall trigger (GAME_core.md 5.2 row 7, which says of
// it "the player's POINT sweep never fires it" -- it is the weapon channel and
// not a bump). It has been wired to SPAC_Impact since the per-event split and
// has never been seen to fire, because nothing in this rig could fire a weapon.
//
// A hitscan is the right instrument: SPAC_Impact is raised from inside the
// trace machinery, gated on the opt-in TRACE_Impact flag (p_trace.cpp:472), and
// P_LineAttack is the path that passes it. The flat probe in roth::UseFlat does
// NOT -- it traces with TRACE_NoSky only -- so a use cannot raise an impact
// trigger as a side effect, which was worth checking before trusting either.
static const int SHOOT_MARKER = -5;
static const double SHOOT_STANDOFF = 40.0;   // units out from the wall
static const int SHOOT_SETTLE_TICS = 3;

// -6 means "shoot every side carrying the trigger opcode in `x`", expanded into
// SHOOT_MARKER entries at drain time. It has to be resolved then and not in the
// console command, because +exec runs before the deferred `map` and a command
// asking the runtime anything finds no level -- the same ordering that makes a
// cfg unable to read the load report.
static const int SHOOT_OP_MARKER = -6;

// -7 / -8: the same stand-off placement, but press USE instead of shooting.
//
// 0x18 is the left-click-wall trigger and the busiest category in the map (26
// records), and it had NEVER been observed firing: every door pose in every
// test so far hit a door LEAF, which short-circuits to SwingDoor before the
// trigger path is reached. "LIVE -- the use key" in the load report was a label
// derived from the wiring, not an observation, which is exactly the kind of
// claim this project keeps catching.
static const int USEWALL_MARKER = -7;
static const int USEWALL_OP_MARKER = -8;

void DoDump(int px, int py, int pang, int w, int h, const char *path);
void PlaceCamera(int px, int py, int pang, int pitch = 0);
void DoSprites(int px, int py, int pang, const char *path);
} // namespace

// Called once per tic from P_Ticker.
void RothDiff_RunPending()
{
	if (g_pending.Size() == 0) return;

	// SAY WHY THE QUEUE IS NOT DRAINING.
	//
	// Both of the guards below are silent, and a rig that queues six captures
	// and then does nothing looks exactly like a frozen game -- which is how an
	// hour went on a run whose queue was simply never eligible. The state is
	// printed once a second while a queue is waiting, so a log always says
	// whether the rig was blocked and on which condition, instead of leaving it
	// to be guessed from the absence of output.
	const bool ready = (gamestate == GS_LEVEL)
		&& (players[consoleplayer].mo != nullptr);
	static int lastReport = -1;
	static bool everRan = false;
	if (!ready)
	{
		if (lastReport != gametic / TICRATE)
		{
			lastReport = gametic / TICRATE;
			Printf("rothdiff: %u queued, WAITING -- gamestate %d (want %d = GS_LEVEL),"
				" player mobj %s\n", g_pending.Size(), (int)gamestate, (int)GS_LEVEL,
				players[consoleplayer].mo != nullptr ? "present" : "NULL");
		}
		return;
	}
	if (!everRan)
	{
		everRan = true;
		Printf("rothdiff: level is up, draining %u queued request(s)\n",
			g_pending.Size());
	}

	// ABANDON THE QUEUE IF THE MAP CHANGES UNDER IT.
	//
	// Every pose in a batch is authored against ONE map: a sector number or an
	// x,y means something different in the next one, or nothing at all. And a
	// map change is not hypothetical here -- the level logic can cause one.
	// Standing in STUDY1's sector 174 fires an 0x13 whose chain warps to
	// STUDY3, so a run that was probing 22 sectors found itself on its sixth
	// probe in a different map and carried on teleporting through numbers that
	// no longer referred to anything. It crashed several probes later, and the
	// crash looked like the dispatch being at fault when the rig had simply
	// driven off the end of its own instructions.
	static FString lastMap;
	const FString nowMap = players[consoleplayer].mo->Level->MapName;
	if (lastMap.IsEmpty())
	{
		lastMap = nowMap;
	}
	else if (lastMap.Compare(nowMap) != 0)
	{
		Printf("rothdiff: MAP CHANGED %s -> %s with %u request(s) left."
			" Abandoning them: poses are authored for one map.\n",
			lastMap.GetChars(), nowMap.GetChars(), g_pending.Size());
		Printf("rothdiff: a trigger chain did this, which is a RESULT, not a"
			" fault -- re-run the rest against the new map on purpose.\n");
		g_pending.Clear();
		lastMap = nowMap;
		AddCommandString("quit");
		return;
	}

	// ONE capture per tic, never the whole queue.
	//
	// A 640x480 dump is 307,200 traces. Draining ten of those in a single tic
	// is three million traces before the frame ends, which in a debug build
	// looks exactly like a hang: the window never paints and the process has to
	// be killed. One per tic keeps the game alive and visibly progressing, and
	// costs nothing that matters -- this is a batch capture, not gameplay.
	PendingDump &front = g_pending[0];

	// A SCREENSHOT NEEDS THE FRAME AFTER THE MOVE, not the frame of the move.
	// The camera is placed, then we wait: the renderer draws from the viewpoint
	// as it was at the start of the tic, so shooting immediately captures the
	// old position. Counted down here rather than slept on, so the game keeps
	// running normally in between.
	if (front.w == SHOOT_OP_MARKER || front.w == USEWALL_OP_MARKER)
	{
		const bool useIt = (front.w == USEWALL_OP_MARKER);
		const uint8_t op = (uint8_t)front.x;
		g_pending.Delete(0);

		// Resolved HERE and not in the command, because the command runs from
		// +exec before the deferred `map` and would find no level.
		const std::vector<int> sides = roth::SidesWithOpcode(op);
		Printf("rothdiff_shoot: opcode 0x%02x is on %u side(s)\n",
			(unsigned)op, (unsigned)sides.size());

		// Pushed to the FRONT, in order, so they run before anything queued
		// after this marker.
		for (size_t i = 0; i < sides.size(); i++)
		{
			PendingDump d;
			d.x = sides[i]; d.y = 0; d.ang = 0;
			d.w = useIt ? USEWALL_MARKER : SHOOT_MARKER; d.h = 0;
			d.screenshot = false;
			d.settle = SHOOT_SETTLE_TICS;
			g_pending.Insert(i, d);
		}
		if (g_pending.Size() > 0) return;
		Printf("rothdiff: all captures done; quitting\n");
		AddCommandString("quit");
		return;
	}

	if (front.w == SHOOT_MARKER || front.w == USEWALL_MARKER)
	{
		const bool useIt = (front.w == USEWALL_MARKER);
		AActor *pm = players[consoleplayer].mo;
		if (front.settle == SHOOT_SETTLE_TICS)
		{
			const int sideIdx = front.x;
			if (sideIdx < 0 || (size_t)sideIdx >= pm->Level->sides.Size()
				|| pm->Level->sides[sideIdx].linedef == nullptr)
			{
				Printf("rothdiff_shoot: side %d is out of range or has no line\n",
					sideIdx);
				g_pending.Delete(0);
				if (g_pending.Size() > 0) return;
				Printf("rothdiff: all captures done; quitting\n");
				AddCommandString("quit");
				return;
			}

			// STAND OFF THE WALL ON THE SIDE THAT OWNS THIS SIDEDEF, facing it.
			//
			// A Doom line's FRONT side (sidedef[0]) lies to the RIGHT of v1->v2,
			// so the outward normal there is (dy, -dx); the back side is the
			// other way. Shooting from the wrong side would hit the far face of
			// the wall, or nothing, and read as the trigger not firing.
			line_t *ln = pm->Level->sides[sideIdx].linedef;
			const DVector2 v1 = ln->v1->fPos(), v2 = ln->v2->fPos();
			const DVector2 mid = (v1 + v2) * 0.5;
			DVector2 d = v2 - v1;
			const double len = d.Length();
			if (len <= 0) { g_pending.Delete(0); return; }
			d /= len;
			DVector2 n(d.Y, -d.X);                       // right of v1->v2
			if (ln->sidedef[1] == &pm->Level->sides[sideIdx]) n = -n;

			const DVector2 from = mid + n * SHOOT_STANDOFF;
			const DAngle face = (mid - from).Angle();
			PlaceCamera((int)from.X, (int)from.Y, 0);
			pm->Angles.Yaw = face;
			pm->Angles.Pitch = nullAngle;
			Printf("rothdiff_shoot: side %d (line %d), standing at (%d,%d)"
				" facing %.1f\n", sideIdx, ln->Index(), (int)from.X, (int)from.Y,
				face.Degrees());

			// SAY WHEN THE STAND-OFF LANDED SOMEWHERE ELSE.
			//
			// 40 units out from a wall's midpoint is inside the neighbouring
			// geometry in a tight spot -- a corner, a narrow nook -- and the
			// player is then shoved before the shot, so the hitscan goes
			// somewhere unintended and the trigger looks dead. It shows in the
			// log only as a stray SPAC_Push (event 0x8) on an adjacent sidedef,
			// which is not obviously a placement failure unless you already
			// know to look for it. Six of 22 shots did this.
			sector_t *want = pm->Level->sides[sideIdx].sector;
			if (pm->Sector != want)
			{
				Printf("rothdiff_shoot: WARNING side %d belongs to sector %d but"
					" the stand-off landed in %d -- this shot proves nothing\n",
					sideIdx, want != nullptr ? want->Index() : -1,
					pm->Sector != nullptr ? pm->Sector->Index() : -1);
			}
		}

		// Let the move settle before firing, so the shot leaves from the pose
		// just set rather than from wherever the player was.
		if (--front.settle > 0) return;

		if (useIt)
		{
			// The use key, the same function +use calls.
			P_UseLines(&players[consoleplayer]);
		}
		else
		{
			// A hitscan with TRACE_Impact behind it. Damage 0 and no puff: the
			// point is the line activation, not hurting anything.
			P_LineAttack(pm, pm->Angles.Yaw, SHOOT_STANDOFF * 2.0,
				pm->Angles.Pitch, 0, NAME_None, NAME_BulletPuff);
		}

		g_pending.Delete(0);
		if (g_pending.Size() > 0) return;
		Printf("rothdiff: all captures done; quitting\n");
		AddCommandString("quit");
		return;
	}

	if (front.w == SECTOR_MARKER || front.w == SECTOR_USE_MARKER)
	{
		const bool pressUse = (front.w == SECTOR_USE_MARKER);
		if (front.settle == SECTOR_SETTLE_TICS)
		{
			const int secnum = front.x;
			AActor *pm = players[consoleplayer].mo;
			if (secnum < 0 || (size_t)secnum >= pm->Level->sectors.Size())
			{
				Printf("rothdiff_sector: %d is out of range (%u sectors)\n",
					secnum, pm->Level->sectors.Size());
				g_pending.Delete(0);
				if (g_pending.Size() > 0) return;
				Printf("rothdiff: all captures done; quitting\n");
				AddCommandString("quit");
				return;
			}

			// centerspot is the sector's own centre, which the engine already
			// maintains for sound and for 3D-floor tests. Good enough to stand
			// in; a concave sector could put it outside the floor, which is
			// visible as a sector number that does not match the one asked for.
			sector_t *sec = &pm->Level->sectors[secnum];
			PlaceCamera((int)sec->centerspot.X, (int)sec->centerspot.Y, 0,
				pressUse ? FLOOR_LOOK_PITCH : 0);

			// LOOK NEARLY STRAIGHT DOWN for the floor test, overriding the ROTH
			// pitch just set.
			//
			// FLOOR_LOOK_PITCH is the original's full-down view pitch, which is
			// only about 44 degrees, and that is not steep enough to clear
			// nearby geometry: in sector 409 the ray met a WALL at z -0.6 about
			// 27 units out before it ever reached the floor, which reads as a
			// broken dispatch and is really a cramped room.
			//
			// Steeper than the game can look is correct HERE and nowhere else.
			// This probe is not modelling the player's view, it is aiming a test
			// ray at a specific sector's floor. The original does not aim with
			// the view centre at all -- it picks under a free CURSOR over the
			// rendered frame, so its reachable aim is far wider than a
			// centre-screen ray, and in VR it will be a hand pointer, which is
			// wider still. A centre ray is the narrowest case, not the real one.
			if (pressUse) pm->Angles.Pitch = DAngle::fromDeg(80.);
			Printf("rothdiff_sector: asked for %d, standing in %d%s\n", secnum,
				pm->Sector != nullptr ? pm->Sector->Index() : -1,
				pressUse ? ", looking down" : "");
		}

		// Wait, so the sector-enter dispatch in P_PlayerThink gets tics to run
		// in before the queue moves on or the game quits.
		if (--front.settle > 0) return;

		// The use goes LAST, after the camera has settled: the view direction
		// the trace reads is the one set above, and a use on the tic of the move
		// would be aiming from the old pose.
		if (pressUse) P_UseLines(&players[consoleplayer]);

		g_pending.Delete(0);
		if (g_pending.Size() > 0) return;
		Printf("rothdiff: all captures done; quitting\n");
		AddCommandString("quit");
		return;
	}

	if (front.w == USE_MARKER)   // press use here, see rothdiff_use
	{
		const PendingDump d = g_pending[0];
		g_pending.Delete(0);
		PlaceCamera(d.x, d.y, d.ang);

		// WHAT IS ACTUALLY THERE. Three separate things have to be true for a
		// use to reach the Realms logic, and a silent failure looks identical
		// for all three: the line must still carry the marker at play time, it
		// must be within USERANGE (64) of the player, and the ray must hit it.
		// Printed so the next guess is not needed.
		AActor *pm = players[consoleplayer].mo;
		int marked = 0, near64 = 0;
		double nearest = 1e9;
		for (auto &ln : pm->Level->lines)
		{
			if (ln.special != 9000) continue;
			marked++;
			const DVector2 mid = (ln.v1->fPos() + ln.v2->fPos()) * 0.5;
			const double dist = (mid - pm->Pos().XY()).Length();
			if (dist < nearest) nearest = dist;
			if (dist <= 64.0) near64++;
		}
		Printf("rothdiff_use: at (%d,%d,%d) sector %d -- %d marked line(s) in"
			" level, %d within 64, nearest %.0f units\n",
			d.x, d.y, d.ang,
			pm->Sector != nullptr ? pm->Sector->Index() : -1,
			marked, near64, nearest);

		P_UseLines(&players[consoleplayer]);
		if (g_pending.Size() > 0) return;
		Printf("rothdiff: all captures done; quitting\n");
		AddCommandString("quit");
		return;
	}

	if (front.w == -1)   // a sprite-rectangle listing, see rothdiff_sprites
	{
		const PendingDump d = g_pending[0];
		g_pending.Delete(0);
		DoSprites(d.x, d.y, d.ang, d.path.GetChars());
		if (g_pending.Size() > 0) return;
		Printf("rothdiff: all captures done; quitting\n");
		AddCommandString("quit");
		return;
	}

	if (front.screenshot)
	{
		PlaceCamera(front.x, front.y, front.ang, front.pitch);
		if (front.settle > 0) { front.settle--; return; }
		// COPY THE WHOLE RECORD, not just the path. `front` is a reference into
		// g_pending, so after Delete(0) it names the NEXT queued capture --
		// which made every shot report the pose of the one after it, and the
		// last one report whatever was left in freed storage. The path was
		// already being rescued for this reason; the three numbers were not,
		// and a two-shot run logged both shots at the second shot's angle.
		const PendingDump shot = front;
		g_pending.Delete(0);
		M_ScreenShot(shot.path.GetChars());
		Printf("rothdiff_shot: %s at (%d,%d,%d)\n", shot.path.GetChars(),
			shot.x, shot.y, shot.ang);
		if (g_pending.Size() > 0) return;
		Printf("rothdiff: all captures done; quitting\n");
		AddCommandString("quit");
		return;
	}

	const PendingDump d = g_pending[0];
	g_pending.Delete(0);
	DoDump(d.x, d.y, d.ang, d.w, d.h, d.path.GetChars());

	if (g_pending.Size() > 0) return;

	// A queued batch only ever comes from a capture run driven by +exec, and
	// such a run must not leave the game sitting open waiting for a human.
	// `quit` cannot go in the script itself: like `exec` it is not deferred, so
	// it would fire at startup and kill the process before `map` ever produced
	// a level -- which is exactly what it did.
	Printf("rothdiff: all captures done; quitting\n");
	AddCommandString("quit");
}

// PRESS USE, WITHOUT A HUMAN. Doors could be opened by the console command all
// day and not by the use key, and the difference between those two paths was
// invisible because only a person in a headset could exercise the second one.
// This places the camera exactly as a capture does and then runs the REAL use
// path -- P_UseLines, the same call the +use button makes -- so the thing that
// was broken is the thing that gets tested.
//
// Camera spots aimed at every door: tools/rothdiff/doorgeom.exe <ROTH> -cam MAP
CCMD(rothdiff_use)
{
	if (argv.argc() != 4)
	{
		Printf("usage: rothdiff_use <x> <y> <angle512>\n");
		Printf("  places the camera and presses use, as the key does\n");
		return;
	}
	// QUEUED, because `+exec` runs BEFORE `map` -- exec is immediate and map is
	// deferred -- so a use asked for in a capture script arrives with no level
	// and was simply dropped. Every other command in this file queues for the
	// same reason; this one did not, and reported "no level" four times.
	PendingDump d;
	d.x = atoi(argv[1]); d.y = atoi(argv[2]); d.ang = atoi(argv[3]);
	d.w = USE_MARKER; d.h = 0;
	d.screenshot = false;
	d.settle = 0;
	g_pending.Push(d);
	Printf("rothdiff_use: queued (%d,%d,%d)\n", d.x, d.y, d.ang);
}

CCMD(rothdiff_dump)
{
	if (argv.argc() != 7)
	{
		Printf("usage: rothdiff_dump <x> <y> <angle512> <width> <height> <file>\n");
		Printf("  captures a per-pixel identity buffer for rothdiff.\n");
		return;
	}

	if (gamestate != GS_LEVEL || players[consoleplayer].mo == nullptr)
	{
		PendingDump d;
		d.x = atoi(argv[1]); d.y = atoi(argv[2]); d.ang = atoi(argv[3]);
		d.w = atoi(argv[4]); d.h = atoi(argv[5]); d.path = argv[6];
		d.screenshot = false;
		d.settle = 0;
		g_pending.Push(d);
		Printf("rothdiff_dump: queued %s until a level is up\n", argv[6]);
		return;
	}

	DoDump(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]),
		atoi(argv[4]), atoi(argv[5]), argv[6]);
}

// Where every prop's picture lands on the ORIGINAL's 640x480 screen, computed
// from what the loader built -- position, scale, offsets -- through the
// original's measured camera (focal 309.77 x 355.06, centre 320.48, 240.54,
// eye 144 above the floor). One line per actor:
//
//     S xl xr ytop ybot depth tex x y z
//
// which is the same rectangle ROTH.C's wall driver is handed for a billboard
// (tools/oracle: ORACLE_WALLLOG, the lines with flags 0019), so the two can be
// compared number for number, without rendering and without anyone's eyes.
CCMD(rothdiff_sprites)
{
	if (argv.argc() != 5)
	{
		Printf("usage: rothdiff_sprites <x> <y> <angle512> <file>\n");
		return;
	}
	PendingDump d;
	d.x = atoi(argv[1]); d.y = atoi(argv[2]); d.ang = atoi(argv[3]);
	d.w = -1; d.h = 0;
	d.path = argv[4];
	d.screenshot = false;
	d.settle = 0;
	g_pending.Push(d);
	Printf("rothdiff_sprites: queued %s\n", argv[4]);
}

// Stand in a sector, named by number, one after another.
//
// A sector is not reachable any other way: the load report says WHICH sectors
// carry a given trigger opcode, but turning a sector number into a point inside
// it needs the built level, and the rest of the rig takes x,y. This is how an
// enter-sector trigger gets tested on purpose instead of by driving past poses
// and hoping one of them lands.
//
// Takes a list, because the interesting question is usually "any of these 23".
CCMD(rothdiff_sector)
{
	if (argv.argc() < 2)
	{
		Printf("usage: rothdiff_sector <sector> [sector ...]\n");
		Printf("  stands in each in turn, pausing %d tics so a per-tic dispatch\n",
			SECTOR_SETTLE_TICS);
		Printf("  has somewhere to run. See the load report for which sectors\n");
		Printf("  carry which trigger opcode.\n");
		return;
	}
	for (int i = 1; i < argv.argc(); i++)
	{
		PendingDump d;
		d.x = atoi(argv[i]); d.y = 0; d.ang = 0;
		d.w = SECTOR_MARKER; d.h = 0;
		d.screenshot = false;
		d.settle = SECTOR_SETTLE_TICS;
		g_pending.Push(d);
	}
	Printf("rothdiff_sector: queued %d sector(s)\n", argv.argc() - 1);
}

// Stand in a sector, look at its FLOOR, and press use -- the 0x19 test.
//
// Separate from rothdiff_sector because that one must NOT press use: it tests
// the enter-sector trigger, and a use there would fire whatever 0x18 happened
// to be on a nearby wall and muddy the log with triggers the test is not about.
CCMD(rothdiff_usefloor)
{
	if (argv.argc() < 2)
	{
		Printf("usage: rothdiff_usefloor <sector> [sector ...]\n");
		Printf("  stands in each, pitches down %d (about 44 degrees) and uses.\n",
			FLOOR_LOOK_PITCH);
		Printf("  See the load report's `0x19 sectors` line for which to pass.\n");
		return;
	}
	for (int i = 1; i < argv.argc(); i++)
	{
		PendingDump d;
		d.x = atoi(argv[i]); d.y = 0; d.ang = 0;
		d.w = SECTOR_USE_MARKER; d.h = 0;
		d.screenshot = false;
		d.settle = SECTOR_SETTLE_TICS;
		g_pending.Push(d);
	}
	Printf("rothdiff_usefloor: queued %d sector(s)\n", argv.argc() - 1);
}

// Shoot a wall, by sidedef -- the 0x1a test.
//
// The load report's `0x1a sides` line says which sidedefs carry one, with the
// line index beside each. Nothing else in this rig can fire a weapon, which is
// why the impact channel has been wired and unobserved since it was split off
// the use path.
CCMD(rothdiff_shoot)
{
	if (argv.argc() < 2)
	{
		Printf("usage: rothdiff_shoot <sidedef> [sidedef ...]\n");
		Printf("         rothdiff_shoot op <opcodeHex>\n");
		Printf("  stands %g units off each wall on the side that owns the\n",
			SHOOT_STANDOFF);
		Printf("  sidedef, faces it and fires a hitscan. `op 1a` resolves every\n");
		Printf("  side carrying that opcode once the level is up, so a script\n");
		Printf("  need not carry sidedef numbers copied out of a previous log.\n");
		return;
	}

	if (argv.argc() == 3 && stricmp(argv[1], "op") == 0)
	{
		PendingDump d;
		d.x = (int)strtol(argv[2], nullptr, 16); d.y = 0; d.ang = 0;
		d.w = SHOOT_OP_MARKER; d.h = 0;
		d.screenshot = false;
		d.settle = 0;
		g_pending.Push(d);
		Printf("rothdiff_shoot: queued every side with opcode 0x%02x\n",
			(unsigned)d.x);
		return;
	}

	for (int i = 1; i < argv.argc(); i++)
	{
		PendingDump d;
		d.x = atoi(argv[i]); d.y = 0; d.ang = 0;
		d.w = SHOOT_MARKER; d.h = 0;
		d.screenshot = false;
		d.settle = SHOOT_SETTLE_TICS;
		g_pending.Push(d);
	}
	Printf("rothdiff_shoot: queued %d sidedef(s)\n", argv.argc() - 1);
}

// Press use against a wall, by sidedef -- the 0x18 test.
//
// Same placement as rothdiff_shoot, the use key instead of a shot. This exists
// because 0x18 is the busiest trigger category in the map and had never been
// seen to fire: every door pose used for testing hits a door LEAF, which
// short-circuits to SwingDoor before the trigger path is reached, so the use
// key had only ever been proven against doors.
CCMD(rothdiff_usewall)
{
	if (argv.argc() < 2)
	{
		Printf("usage: rothdiff_usewall <sidedef> [sidedef ...]\n");
		Printf("         rothdiff_usewall op <opcodeHex>\n");
		Printf("  stands %g units off the wall, faces it and presses use.\n",
			SHOOT_STANDOFF);
		Printf("  `op 18` resolves every side carrying that opcode.\n");
		return;
	}

	if (argv.argc() == 3 && stricmp(argv[1], "op") == 0)
	{
		PendingDump d;
		d.x = (int)strtol(argv[2], nullptr, 16); d.y = 0; d.ang = 0;
		d.w = USEWALL_OP_MARKER; d.h = 0;
		d.screenshot = false;
		d.settle = 0;
		g_pending.Push(d);
		Printf("rothdiff_usewall: queued every side with opcode 0x%02x\n",
			(unsigned)d.x);
		return;
	}

	for (int i = 1; i < argv.argc(); i++)
	{
		PendingDump d;
		d.x = atoi(argv[i]); d.y = 0; d.ang = 0;
		d.w = USEWALL_MARKER; d.h = 0;
		d.screenshot = false;
		d.settle = SHOOT_SETTLE_TICS;
		g_pending.Push(d);
	}
	Printf("rothdiff_usewall: queued %d sidedef(s)\n", argv.argc() - 1);
}

// A picture, from the same camera spot as an identity buffer, for the things
// numbers do not catch: lighting, framing, mood.
CCMD(rothdiff_shot)
{
	if (argv.argc() != 5 && argv.argc() != 6)
	{
		Printf("usage: rothdiff_shot <x> <y> <angle512> <file> [pitchROTH]\n");
		Printf("  pitchROTH: the original's view pitch (+-126, positive looks up); its shear\n");
		Printf("  of pitch*FY/128 px is a real pitch of atan(pitch/128)\n");
		return;
	}
	PendingDump d;
	d.x = atoi(argv[1]); d.y = atoi(argv[2]); d.ang = atoi(argv[3]);
	d.w = d.h = 0;
	d.path = argv[4];
	d.pitch = argv.argc() == 6 ? atoi(argv[5]) : 0;
	d.screenshot = true;
	// TWO SECONDS, not two tics. Two was enough for the camera move to take
	// effect, but the startup console is still down over the view that early
	// and lands in the picture -- which makes the shot useless for the one job
	// it has, being compared against ROTH.C's.
	d.settle = 70;
	g_pending.Push(d);
	Printf("rothdiff_shot: queued %s\n", argv[4]);
}

namespace
{
// STAND THE CAMERA ON THE FLOOR IT HAS JUST BEEN MOVED OVER.
//
// A pose names a spot on the map, not a height. Moving in x and y while keeping
// the z of wherever the camera stood before leaves it buried in the floor, or
// up inside a wall, the moment two poses sit at different floor heights -- and
// a camera inside solid space traces a WALL on every single ray.
//
// That is not a subtle error. It reads in a rothdiff report as "the loader
// built a wall where Realms has floor", across the whole picture, on every
// pose, which is exactly the kind of false lead this rig exists to prevent.
// Measured 2026-09-28: 100% of rays hit wall before this, 0.00% match.
void StandOnFloor(AActor *cam, int px, int py)
{
	if (cam->Sector == nullptr) return;
	cam->SetZ(cam->Sector->floorplane.ZatPoint((double)px, (double)py));
}

// Put the camera exactly where it is asked for. Shared so a screenshot and an
// identity buffer are taken from provably the same place -- the whole value of
// a side-by-side is that only one thing differs between the two pictures.
void PlaceCamera(int px, int py, int pang, int pitch)
{
	AActor *cam = players[consoleplayer].mo;
	if (cam == nullptr) return;
	cam->SetOrigin(DVector3(px, py, cam->Z()), true);
	StandOnFloor(cam, px, py);
	cam->Angles.Yaw = RothAngleToDoom(pang);
	// Doom pitch is positive looking DOWN.
	cam->Angles.Pitch = DAngle::fromRad(-atan(pitch / 128.0));
	// The renderer interpolates between the previous tic's viewpoint and this
	// one, so without clearing that the shot is taken part-way through a very
	// long jump from wherever the player was standing.
	cam->ClearInterpolation();
}

void DoSprites(int px, int py, int pang, const char *path)
{
	AActor *cam = players[consoleplayer].mo;
	if (cam == nullptr) return;
	PlaceCamera(px, py, pang);
	FILE *f = fopen(path, "w");
	if (f == nullptr) { Printf("rothdiff_sprites: could not write %s\n", path); return; }

	const double FX = 309.77, FY = 355.06, X0 = 320.48, Y0 = 240.54;
	const DAngle ang = RothAngleToDoom(pang);
	const DVector2 fwd = ang.ToVector();
	const DVector2 right(fwd.Y, -fwd.X);
	const double eyeZ = cam->Z() + 144.0;
	int n = 0;

	auto it = cam->Level->GetThinkerIterator<AActor>();
	while (AActor *mo = it.Next())
	{
		if (mo == cam || mo->player != nullptr) continue;
		if (mo->sprite < 0 || (unsigned)mo->sprite >= sprites.Size()) continue;
		const spritedef_t &sdef = sprites[mo->sprite];
		if (sdef.numframes == 0) continue;
		const spriteframe_t &sfr = SpriteFrames[sdef.spriteframes + (mo->frame < sdef.numframes ? mo->frame : 0)];
		FGameTexture *tex = TexMan.GetGameTexture(sfr.Texture[0]);
		if (tex == nullptr || !tex->isValid()) continue;

		const DVector2 rel(mo->X() - px, mo->Y() - py);
		const double depth = rel.X * fwd.X + rel.Y * fwd.Y;
		if (depth < 16.0) continue;
		const double lat = rel.X * right.X + rel.Y * right.Y;
		const double w = tex->GetDisplayWidth() * mo->Scale.X, h = tex->GetDisplayHeight() * mo->Scale.Y;
		const double lo = tex->GetDisplayLeftOffset() * mo->Scale.X, to = tex->GetDisplayTopOffset() * mo->Scale.Y;
		const double zTop = mo->Z() + to, zBot = zTop - h;
		const double xl = X0 + FX * (lat - lo) / depth, xr = X0 + FX * (lat - lo + w) / depth;
		const double yt = Y0 - FY * (zTop - eyeZ) / depth, yb = Y0 - FY * (zBot - eyeZ) / depth;
		// HOW MANY DISTINCT VIEWS THIS PROP HAS, and which one this camera gets.
		// A directional prop is one whose frame carries more than one distinct
		// rotation texture; everything else repeats rotation 0. Printed because
		// "which props are directional" is otherwise only answerable by reading
		// the art tables offline and cross-referencing by entry id, and the
		// whole question being asked of this dump is whether the view ORDER is
		// right (HANDOFF_REMAROTH.md section 7).
		// COUNT DISTINCT TEXTURES ACROSS ALL 16 SLOTS. Not Texture[0] against
		// Texture[1]: an eight-view sprite fills the sixteen slots PAIRWISE, so
		// those two are equal on a directional prop too and the first version of
		// this check reported every prop in STUDY2 as non-directional.
		int rots = 0;
		for (int r = 0; r < 16; r++)
		{
			if (!sfr.Texture[r].isValid()) continue;
			bool seen = false;
			for (int q = 0; q < r; q++)
				if (sfr.Texture[q] == sfr.Texture[r]) { seen = true; break; }
			if (!seen) rots++;
		}
		// GZDoom's own choice for this view (hw_sprites.cpp:1436): the angle
		// from the prop to the camera, biased by half a step, in 16ths.
		const DAngle toCam = DVector2(px - mo->X(), py - mo->Y()).Angle();
		const unsigned rot = (unsigned)((toCam - mo->Angles.Yaw
			+ DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28) & 15u;
		FGameTexture *rtex = TexMan.GetGameTexture(sfr.Texture[rots > 8 ? rot : (rot >> 1)]);

		fprintf(f, "S %.1f %.1f %.1f %.1f %.1f %s %.1f %.1f %.1f rots=%d rot=%u view=%s\n",
			xl, xr, yt, yb, depth,
			tex->GetName().GetChars(), mo->X(), mo->Y(), mo->Z(),
			rots, rots > 8 ? rot : (rot >> 1),
			(rtex != nullptr && rtex->isValid()) ? rtex->GetName().GetChars() : "-");
		n++;
	}
	fclose(f);
	Printf("rothdiff_sprites: %s  %d props at (%d,%d,%d)\n", path, n, px, py, pang);
}

void DoDump(int px, int py, int pang, int w, int h, const char *path)
{
	AActor *cam = players[consoleplayer].mo;
	if (cam == nullptr)
	{
		Printf("rothdiff_dump: no player to place.\n");
		return;
	}

	if (w < 1 || h < 1 || w > 4096 || h > 4096)
	{
		Printf("rothdiff_dump: silly buffer size %dx%d\n", w, h);
		return;
	}

	// PLACE THE CAMERA EXACTLY. A comparison is only meaningful if both engines
	// are at the same point, so the position is set rather than walked to.
	const DAngle ang = RothAngleToDoom(pang);
	cam->SetOrigin(DVector3(px, py, cam->Z()), true);
	StandOnFloor(cam, px, py);
	cam->Angles.Yaw = ang;

	// Eye height from the player's own viewheight, not from viewz: viewz is
	// recomputed by the player think and would still be the height of wherever
	// the camera was BEFORE this command moved it.
	//
	// NOTE this is the engine's eye height, not yet ROTH's. The original's rule
	// is player Z + 2 * metadata[+0x0A] (144 on most maps), clamped 24 units
	// from floor and ceiling (ROTH_SURFACES_FIX.md 3.7, player.c:76-81,371-383).
	// Until that is implemented and tested, a vertical mismatch in a rothdiff
	// report may be eye height rather than anything about textures.
	const double eyeZ = cam->Z() + (cam->player ? cam->player->viewheight : cam->Height * 0.75);
	const DVector3 eye(px, py, eyeZ);

	// The ray basis. Horizontal half-extent comes from ROTH's focal length;
	// the vertical follows from the pixel being square, which is why
	// pixelratio is 1.0 (see vrealms_iwad/MAPINFO).
	const double tanHalfH = (ROTH_FOCAL_DEN / ROTH_FOCAL_NUM) * 0.5;
	const double aspect = (double)w / (double)h;
	const double tanHalfV = tanHalfH / aspect;

	const DVector2 fwd = ang.ToVector();
	const DVector2 right(fwd.Y, -fwd.X);   // screen +x is to the camera's right

	std::vector<IdPixel> buf((size_t)w * (size_t)h);

	int hitFloor = 0, hitCeil = 0, hitWall = 0, hitNone = 0;

	for (int y = 0; y < h; y++)
	{
		// +1 puts the sample at the pixel CENTRE. Sampling the corner biases
		// every edge by half a pixel, which at 320 wide is a whole texel on a
		// 2-units-per-texel flat.
		const double sy = (1.0 - 2.0 * ((y + 0.5) / h)) * tanHalfV;
		for (int x = 0; x < w; x++)
		{
			const double sx = (2.0 * ((x + 0.5) / w) - 1.0) * tanHalfH;
			const DVector3 dir(fwd.X + right.X * sx, fwd.Y + right.Y * sx, sy);

			FTraceResults res;
			IdPixel p = { 0, 0, 0, 0 };

			if (Trace(eye, cam->Sector, dir, 32768.0, ActorFlags::FromInt(0), 0xFFFFFFFF,
				cam, res, TRACE_HitSky))
			{
				switch (res.HitType)
				{
				case TRACE_HitFloor:
					hitFloor++;
					p.flags = FLAGS_SPRITE_PATH;
					p.tex = res.Sector ? res.Sector->GetTexture(sector_t::floor).GetIndex() : 0;
					p.fill = p.tex ? FILL_FLOOR_TEX : FILL_FLOOR_SOLID;
					p.id = res.Sector ? (uint16_t)res.Sector->Index() : 0;
					break;

				case TRACE_HitCeiling:
					hitCeil++;
					p.flags = FLAGS_SPRITE_PATH;
					p.tex = res.Sector ? res.Sector->GetTexture(sector_t::ceiling).GetIndex() : 0;
					p.fill = p.tex ? FILL_CEIL_TEX : FILL_CEIL_SOLID;
					p.id = res.Sector ? (uint16_t)res.Sector->Index() : 0;
					break;

				case TRACE_HitWall:
					// OUT OF SCOPE FOR NOW, and left visibly so. Walls have
					// their own fill word in the original which has not been
					// read yet, and inventing one here would put a made-up
					// number into a file whose whole purpose is to be trusted.
					// rothdiff scores flats; wall pixels are reported as
					// out-of-scope on both sides.
					hitWall++;
					p.flags = 0xFFFF;
					p.tex = res.HitTexture.GetIndex();
					p.id = res.Line ? (uint16_t)res.Line->Index() : 0;
					break;

				default:
					hitNone++;
					break;
				}
			}
			else
			{
				hitNone++;
			}

			buf[(size_t)y * (size_t)w + (size_t)x] = p;
		}
	}

	if (!WriteRidb(path, w, h, px, py, pang, buf))
	{
		Printf("rothdiff_dump: could not write %s\n", path);
		return;
	}
	Printf("rothdiff_dump: %s  %dx%d at (%d,%d,%d)\n", path, w, h, px, py, pang);
	Printf("  floor %d  ceiling %d  wall %d  nothing %d\n",
		hitFloor, hitCeil, hitWall, hitNone);
}
} // namespace
