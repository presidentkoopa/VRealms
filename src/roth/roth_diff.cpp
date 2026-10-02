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
