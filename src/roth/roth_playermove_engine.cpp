//
// The original's player movement driving the engine's Realms player (roth_playermove_engine.h).
//
// THE RULES ARE NOT HERE. They are roth_playermove.cpp, engine-free and diffed tick by tick against
// the running original (tools/move_test). This file only:
//   - keeps the port's copy of the original's memory current from the live level (sector and
//     platform heights, +0x0c, the door bits and leaves, the objects, the creature pool, the 0x41
//     shift, the dialogue freeze, the health);
//   - turns the engine's command into the original's held keys (flat keys, sticks, jump, crouch;
//     the VR real crouch) and folds the engine's yaw (mouse, headset, snap turn) into its angle;
//   - runs two 70 Hz ticks and one frame of fts 2 a tic (GAME_core.md 2.3, option (a));
//   - writes the result to the pawn: position (wrap- and room-shifted back to engine space), yaw,
//     eye height.
// docs/PLAYER_MOVEMENT_PORT.md lists each part with its source line and what is and is not wired.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_playermove_engine.h"
#include "roth_playermove.h"
#include "roth_actors_engine.h"
#include "roth_runtime.h"
#include "roth_raw.h"
#include "roth_log.h"
#include "roth_install.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "playsim/p_local.h"
#include "printf.h"
#include "c_cvars.h"
#include "hw_vrmodes.h"
#include "d_event.h"

#include <math.h>
#include <map>
#include <utility>

EXTERN_CVAR(Bool, cl_run)

// NOT ARCHIVED, any of them (CLAUDE.md: the ini is shared with the hands build).
// roth_move_log 1: one line a tic with the original's position, angle, sector, heights, health.
CVAR(Int, roth_move_log, 0, 0)
// The VR real crouch: the head below this fraction of its measured standing height holds the
// original's crouch key (Z). The original's crouched eye is 0x30 of 0x90, a third.
CVAR(Float, roth_vr_crouch_fraction, 0.75f, 0)
// The VR snap turn on Realms levels, degrees (owner, 2026-11-13: 45). 0 leaves vr_snapTurn.
CUSTOM_CVAR(Float, roth_vr_snapturn, 45.f, CVAR_NOINITCALL)
{
	if (roth::PlayerMoveOwnsTurnKeys()) VR_SetSnapTurnOverride(self);
}

namespace roth
{

namespace
{

struct Host : PlayerMoveHost
{
	std::vector<std::pair<int, int>> obRef;   // ob offset / 16 -> (sector, index), for the log
	void DamageDirect(uint32_t damage) override
	{
		ActorsDamagePlayerDirect(damage);
		Printf("[roth_move] fall damage %u (apply_direct_damage_to_player), health now %d\n", damage, (int)PlayerHealth());
	}
	void LandingSound(uint32_t sound) override
	{
		// start_persistent_looping_sound (player.c:292-295): not wired. 0x7fe50 is 0 in STUDY1's new
		// game (tools/move_test INIT thud=0000), so the original plays nothing there either.
		Printf("[roth_move] landing thud %u (not wired)\n", sound);
	}
	void ContactTrigger(uint32_t ob) override
	{
		// fire_tracked_object_trigger (raw_commands.c:2974): not ported yet.
		static uint32_t last = 0xffffffffu;
		if (ob != last) Printf("[roth_move] contact trigger of object at ob+%x (fire_tracked_object_trigger: not wired)\n", ob);
		last = ob;
	}
};

struct State
{
	std::vector<uint8_t> raw;
	PlayerMove pm;
	Host host;
	bool active = false, synced = false;
	FLevelLocals *level = nullptr;
	const Map *map = nullptr;
	int wrapX = 0, wrapY = 0;
	// the command, taken in P_PlayerThink
	uint32_t held = 0;
	bool shift = false, crouchToggle = false;
	// the yaw the engine applied that the original's angle has not taken yet (512ths of a turn)
	double yawFrac = 0.;
	DAngle lastYaw = nullAngle;
	DVector3 lastPos = { 0., 0., 0. };
	double lastShiftX = 0., lastShiftY = 0.;
	// the headset's moves since the last frame
	double roomX = 0., roomY = 0.;
	bool teleport = false;
	DVector2 teleportTo = { 0., 0. };
	int tics = 0;
	bool tuned = false;          // the first map's tuning read (once a session)
	bool sessionTuned = false;
} g_move;

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

AActor *Pawn(FLevelLocals *level)
{
	if (level == nullptr || consoleplayer < 0) return nullptr;
	player_t *p = &players[consoleplayer];
	return p->mo != nullptr && p->mo->Level == level ? (AActor *)p->mo : nullptr;
}

int16_t W16(int v) { return (int16_t)(uint16_t)(v & 0xffff); }

// engine x,y in a Realms sector -> the original's map space (the loader's wrap shift and the room
// shift of stacked rooms taken off)
void ToOriginal(int sectorIndex, double ex, double ey, double &ox, double &oy)
{
	double dx = 0., dy = 0.;
	if (sectorIndex >= 0) SectorShift(sectorIndex, dx, dy);
	ox = ex - dx - g_move.wrapX;
	oy = ey - dy - g_move.wrapY;
}

// The live parts of the original's memory, from the level as it stands.
void SyncWorld()
{
	PlayerMove &pm = g_move.pm;
	const Map &m = *g_move.map;
	const int n = pm.SectorCount() < (int)m.sectors.size() ? pm.SectorCount() : (int)m.sectors.size();
	pm.doors.clear();
	for (int i = 0; i < n; i++)
	{
		const Sector &s = m.sectors[i];
		pm.SetSectorHeights(i, s.ceilingHeight, s.floorHeight);
		pm.SetSectorTextureOverride(i, s.textureMapOverride);
		if (s.platformIndex >= 0 && (size_t)s.platformIndex < m.platforms.size())
			pm.SetPlatformHeights(i, m.platforms[s.platformIndex].undersideZ, m.platforms[s.platformIndex].topZ);
		if (s.IsDoorCapable())
		{
			// a door spawned for this sector: its byte +0x16 bit 0 (doors.c:464, kept once set) and a
			// door record with its leaf (find_record_by_id, find_door_by_sector)
			const bool open = RealmsDoorOpen(i);
			if (open) pm.SetSectorDoorFlag(i, true);
			double xs[4], ys[4];
			if (open && RealmsDoorLeaf(i, xs, ys))
			{
				MoveDoor d;
				d.sector = pm.SectorOffset(i);
				for (int k = 0; k < 4; k++)
				{
					double ox, oy;
					ToOriginal(i, xs[k], ys[k], ox, oy);
					d.x[k] = W16((int)lround(ox)); d.y[k] = W16((int)lround(oy));
				}
				pm.doors.push_back(d);
			}
		}
	}
	// the objects buffer (0x90aa4) rebuilt from the live objects: a word per sector from +2 (the
	// sector's list offset, 0 = none), then each list: a count byte, a pad byte, 16 bytes an object
	std::vector<uint8_t> &ob = pm.ob;
	ob.assign(2 + (size_t)n * 2, 0);
	g_move.host.obRef.clear();
	std::map<std::pair<int, int>, uint32_t> where;
	for (int i = 0; i < n && (size_t)i < m.objects.size(); i++)
	{
		const std::vector<Object> &os = m.objects[i];
		if (os.empty()) continue;
		const size_t at = ob.size();
		if (at > 0xffff) break;
		ob[2 + i * 2] = (uint8_t)at; ob[3 + i * 2] = (uint8_t)(at >> 8);
		ob.push_back((uint8_t)(os.size() > 255 ? 255 : os.size()));
		ob.push_back(0);
		for (size_t k = 0; k < os.size() && k < 255; k++)
		{
			const Object &o = os[k];
			uint8_t r[16];
			auto put = [&](int off, uint16_t v) { r[off] = (uint8_t)v; r[off + 1] = (uint8_t)(v >> 8); };
			put(0, (uint16_t)(o.x - g_move.wrapX)); put(2, (uint16_t)(o.y - g_move.wrapY));
			r[4] = o.textureIndex; r[5] = o.textureSource; r[6] = o.rotation; r[7] = o.flags; r[8] = o.light; r[9] = o.renderType;
			put(0xa, (uint16_t)o.z); put(0xc, o.unk0C); put(0xe, o.commandID);
			where[std::make_pair(i, (int)k)] = (uint32_t)ob.size();
			ob.insert(ob.end(), r, r + 16);
		}
	}
	std::vector<std::pair<int, int>> pool;
	ActorsPoolObjects(pool);
	pm.pool.clear();
	for (const auto &p : pool)
	{
		auto it = where.find(p);
		pm.pool.push_back(it != where.end() ? it->second : 0);
	}
	if (const std::vector<uint8_t> *d = ActorsCollisionDims()) pm.dims = *d;
	pm.speedShift = RealmsSpeedShift();
	RealmsDialogueFreeze(pm.dialogueBusy, pm.freezeGate);
	pm.health = PlayerHealth();
	pm.movementMode = 1;
	pm.runToggle = cl_run ? 1u : 0u;
}

// Take the pawn as it stands: the first tic, and after anything else moved it (an arrival, a warp,
// a savegame).
void Resync(AActor *mo)
{
	PlayerMove &pm = g_move.pm;
	const int si = mo->Sector != nullptr ? mo->Sector->Index() : -1;
	double ox, oy;
	ToOriginal(si, mo->X(), mo->Y(), ox, oy);
	const int16_t ix = W16((int)floor(ox)), iy = W16((int)floor(oy)), iz = W16((int)floor(mo->Z()));
	const uint16_t hint = (si >= 0 && si < pm.SectorCount()) ? pm.SectorOffset(si) : 0;
	pm.sector = pm.Locate(ix, iy, iz, hint);
	pm.sectorCache = pm.sector;
	pm.x = (uint32_t)(int32_t)lround(ox * 65536.);
	pm.y = (uint32_t)(int32_t)lround(oy * 65536.);
	pm.z = (uint32_t)(int32_t)lround(mo->Z() * 65536.);
	const double a = (mo->Angles.Yaw.Degrees() - 90.) * 512. / 360.;
	const double whole = floor(a);
	pm.angle = (uint16_t)(int)whole;
	g_move.yawFrac = a - whole;
	pm.ResetQueues();
	g_move.lastYaw = mo->Angles.Yaw;
	g_move.synced = true;
	g_move.roomX = g_move.roomY = 0.;
	Printf("[roth_move] taken from the pawn: x=%d y=%d z=%d angle=%u sector %d\n", ix, iy, iz, pm.angle, pm.SectorIndex(pm.sector));
}

void WritePawn(AActor *mo, bool teleported)
{
	PlayerMove &pm = g_move.pm;
	const int si = pm.SectorIndex(pm.sector);
	double dx = 0., dy = 0.;
	if (si >= 0) SectorShift(si, dx, dy);
	const DVector3 pos((int32_t)pm.x / 65536. + g_move.wrapX + dx, (int32_t)pm.y / 65536. + g_move.wrapY + dy, (int32_t)pm.z / 65536.);
	mo->SetOrigin(pos, true);
	if (teleported || dx != g_move.lastShiftX || dy != g_move.lastShiftY) mo->ClearInterpolation();
	g_move.lastShiftX = dx; g_move.lastShiftY = dy;
	mo->Vel.Zero();
	mo->Angles.Yaw = DAngle::fromDeg(90. + ((int16_t)pm.angle + g_move.yawFrac) * 360. / 512.);
	if (player_t *p = mo->player)
	{
		// the eye: player_z + 0x8c112 (update_view_transform_params, player.c:80-81)
		p->viewheight = pm.camHeight;
		p->deltaviewheight = 0;
		p->viewz = pos.Z + pm.camHeight;
	}
	g_move.lastPos = mo->Pos();
	g_move.lastYaw = mo->Angles.Yaw;
}

} // namespace

void PlayerMoveSetRaw(const std::vector<uint8_t> &bytes) { g_move.raw = bytes; }

void PlayerMoveBeginLevel(const Map &map, FLevelLocals *level, Log *log)
{
	g_move.active = false;
	g_move.synced = false;
	g_move.level = level;
	g_move.map = &map;
	g_move.wrapX = map.wrapShiftX; g_move.wrapY = map.wrapShiftY;
	g_move.held = 0; g_move.shift = false; g_move.crouchToggle = false;
	g_move.teleport = false; g_move.roomX = g_move.roomY = 0.;
	g_move.tics = 0;
	// The player's state is the game's, not the map's (the original's globals live across maps): only
	// the image is rebuilt here. Position, angle and sector are taken from the pawn on the first tic.
	PlayerMove &pm = g_move.pm;
	pm.host = &g_move.host;
	if (!pm.BuildImage(g_move.raw.data(), g_move.raw.size()))
	{
		if (log != nullptr) log->Line("player movement: the RAW image could not be built; the engine's own movement stays");
		Printf("[roth_move] the RAW image could not be built: the engine's own movement stays\n");
		return;
	}
	g_move.tuned = false;
	g_move.active = true;
	if (roth_vr_snapturn > 0.f) VR_SetSnapTurnOverride(roth_vr_snapturn);
	if (log != nullptr) log->Line("player movement: the original's (roth_playermove), %d sectors in its image", pm.SectorCount());
}

void PlayerMoveEndLevel()
{
	g_move.active = false;
	g_move.synced = false;
	g_move.level = nullptr;
	g_move.map = nullptr;
}

bool PlayerMoveOwns(const player_t *player)
{
	return g_move.active && player != nullptr && player->mo != nullptr && player->mo->Level == g_move.level
		&& player == &players[consoleplayer];
}

bool PlayerMoveOwnsTurnKeys() { return g_move.active; }

void PlayerMoveTakeCmd(player_t *player)
{
	if (!PlayerMoveOwns(player)) return;
	usercmd_t &cmd = player->cmd;
	const int DZ = 0x1000;      // a stick's or key's move counts as the key when past 1/8 of full
	uint32_t h = 0;
	if (cmd.forwardmove > DZ) h |= 1u << MK_FORWARD;
	if (cmd.forwardmove < -DZ) h |= 1u << MK_BACK;
	if (cmd.sidemove > DZ) h |= 1u << MK_STRAFE_RIGHT;
	if (cmd.sidemove < -DZ) h |= 1u << MK_STRAFE_LEFT;
	if (!(cmd.buttons & BT_STRAFE))
	{
		if (cmd.buttons & BT_LEFT) h |= 1u << MK_TURN_LEFT;
		if (cmd.buttons & BT_RIGHT) h |= 1u << MK_TURN_RIGHT;
	}
	if (cmd.buttons & BT_JUMP) h |= 1u << MK_JUMP;
	// crouch: the held key, the engine's crouch toggle (consumed here), the headset's real crouch
	if (player->crouching != 0 && player->crouching != 10) { g_move.crouchToggle = !g_move.crouchToggle; }
	player->crouching = 0;
	bool crouch = (cmd.buttons & BT_CROUCH) != 0 || g_move.crouchToggle;
	if (VRMode::GetVRModeCached(true)->IsVR())
	{
		const float stand = VR_MeasuredHeadHeight(), now = VR_CurrentHeadHeight();
		if (stand > 0.f && now > 0.f && now < stand * roth_vr_crouch_fraction) crouch = true;
	}
	if (crouch) h |= 1u << MK_CROUCH;
	g_move.held = h;
	g_move.shift = (cmd.buttons & BT_SPEED) != 0;

	// the engine moves nothing: no thrust, no jump, no crouch, no gravity (the yaw and pitch stay
	// with the engine; the yaw is folded into the original's angle every tic)
	cmd.forwardmove = cmd.sidemove = cmd.upmove = 0;
	cmd.buttons &= ~(BT_JUMP | BT_CROUCH);
	player->mo->flags |= MF_NOGRAVITY;
	player->mo->Vel.Zero();
}

bool PlayerMoveRenderMove(player_t *player, bool teleport, double dx, double dy)
{
	if (!PlayerMoveOwns(player)) return false;
	if (teleport)
	{
		g_move.teleport = true;
		g_move.teleportTo = player->mo->Pos().XY() + DVector2(dx, dy);
	}
	else
	{
		g_move.roomX += dx;
		g_move.roomY += dy;
	}
	return true;
}

void PlayerMoveTick(FLevelLocals *level)
{
	if (!g_move.active || level != g_move.level || g_move.map == nullptr) return;
	AActor *mo = Pawn(level);
	if (mo == nullptr) return;
	PlayerMove &pm = g_move.pm;

	// init_movement_tuning_from_first_map (map_load.c:197-209) runs for the game's FIRST map only, at
	// start-up and on a new game (game_core.c:1071, :467); its MOVE_SPEED, height, climb and fit stay
	// for the whole game. The retail maps disagree (STUDY1 5/72/32/48, CHURCH1 7/72/32/48, MAZE
	// 9/72/32/24 ...), so it matters which map they come from.
	if (!g_move.sessionTuned)
	{
		const std::string first = RealmsFirstMapName();
		std::vector<uint8_t> bytes;
		if (!first.empty()) bytes = Install::ReadWholeFile(TheInstall().MapFile(first.c_str()));
		PlayerMove tmp;
		if (!bytes.empty() && tmp.BuildImage(bytes.data(), bytes.size()))
		{
			tmp.InitTuning();
			pm.moveSpeed = tmp.moveSpeed; pm.maxClimb = tmp.maxClimb; pm.minFit = tmp.minFit; pm.restHeight = tmp.restHeight;
			pm.height = tmp.height; pm.camHeight = tmp.camHeight;
			g_move.sessionTuned = true;
			Printf("[roth_move] tuning from the first map %s: MOVE_SPEED %u, height %d, max climb %u, min fit %u\n",
				first.c_str(), pm.moveSpeed, (int)pm.restHeight, pm.maxClimb, pm.minFit);
		}
		else if (!g_move.tuned)
		{
			// no first map known: this map's own block, said out loud
			pm.InitTuning();
			Printf("[roth_move] the first map is not known: tuning from this map (MOVE_SPEED %u, height %d, max climb %u, min fit %u)\n",
				pm.moveSpeed, (int)pm.restHeight, pm.maxClimb, pm.minFit);
		}
		g_move.tuned = true;
	}

	SyncWorld();

	// Anything but this file moved the pawn (an arrival, a warp, a load): take it from there.
	if (!g_move.synced || (mo->Pos() - g_move.lastPos).Length() > 0.5) Resync(mo);

	// The engine's yaw since the last tic (the mouse, the headset, the snap turn) into the angle.
	{
		const double d = deltaangle(g_move.lastYaw, mo->Angles.Yaw).Degrees() * 512. / 360.;
		g_move.yawFrac += d;
		const double whole = floor(g_move.yawFrac);
		pm.angle = (uint16_t)(pm.angle + (int)whole);
		g_move.yawFrac -= whole;
	}

	bool teleported = false;
	if (g_move.teleport)
	{
		// The headset's teleport (an option, owner 2026-11-13): to the floor of the sector holding the
		// target, as the original's own relocation puts a warped player (map_load.c:528-554 keeps the
		// height above the floor; this stands him on it). Not through the collision: it is a jump.
		g_move.teleport = false;
		const sector_t *ts = level->PointInSector(g_move.teleportTo);
		double ox, oy;
		ToOriginal(ts != nullptr ? ts->Index() : -1, g_move.teleportTo.X, g_move.teleportTo.Y, ox, oy);
		const uint16_t s = pm.Locate(W16((int)floor(ox)), W16((int)floor(oy)), W16((int32_t)pm.z >> 16), pm.sector);
		if (s != 0)
		{
			pm.sector = s;
			pm.x = (uint32_t)(int32_t)lround(ox * 65536.);
			pm.y = (uint32_t)(int32_t)lround(oy * 65536.);
			pm.z = (uint32_t)((uint32_t)(uint16_t)pm.SectorFloor(s) << 16);
			pm.ResetQueues();
			teleported = true;
		}
	}

	// player_movement_tick twice (the 70 Hz ISR), then one frame of move_player_with_collision with
	// fts 2 (GAME_core.md 2.3 option (a)).
	MoveInput in;
	in.held = g_move.held;
	in.shift = g_move.shift;
	pm.Tick(in);
	pm.Tick(in);
	if (g_move.roomX != 0. || g_move.roomY != 0.)
	{
		if (pm.AddExtraDelta((int32_t)lround(g_move.roomX * 65536.), (int32_t)lround(g_move.roomY * 65536.)))
			g_move.roomX = g_move.roomY = 0.;
	}
	pm.Frame(2);
	pm.ClampViewPitch();
	WritePawn(mo, teleported);
	g_move.tics++;

	if (roth_move_log)
	{
		char bx[32], by[32], bz[32];
		Printf("[roth_move] tic %d x=%s y=%s z=%s angle=%u sector=%d height=%u eye=%u fall=%d,%d jump=%d crouch=%d keys=%03x health=%d\n",
			level->maptime, Fix4(pm.x, bx), Fix4(pm.y, by), Fix4(pm.z, bz), pm.angle & 0x1ff,
			pm.SectorIndex(pm.sector), pm.height, pm.camHeight, pm.fallState, pm.fallAccum, pm.jumpPhase, pm.crouchMode, g_move.held,
			(int)PlayerHealth());
	}
}

} // namespace roth
