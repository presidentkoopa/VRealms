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

// ROTH angle units: 512 per full turn, and 0 points along +Y with 128 at +X
// (clockwise, +Y up) -- see ROTH_SURFACES_FIX.md 3.4 and renderer.c:6093-6126.
// Doom angles run counter-clockwise from +X, hence the negation.
inline DAngle RothAngleToDoom(int a512)
{
	return DAngle::fromDeg(90.0 - (double)a512 * (360.0 / 512.0));
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

void DoDump(int px, int py, int pang, int w, int h, const char *path);
void PlaceCamera(int px, int py, int pang, int pitch = 0);
} // namespace

// Called once per tic from P_Ticker.
void RothDiff_RunPending()
{
	if (g_pending.Size() == 0) return;
	if (gamestate != GS_LEVEL) return;
	if (players[consoleplayer].mo == nullptr) return;

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
	if (front.screenshot)
	{
		PlaceCamera(front.x, front.y, front.ang, front.pitch);
		if (front.settle > 0) { front.settle--; return; }
		const FString path = front.path;
		g_pending.Delete(0);
		M_ScreenShot(path.GetChars());
		Printf("rothdiff_shot: %s at (%d,%d,%d)\n", path.GetChars(),
			front.x, front.y, front.ang);
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
