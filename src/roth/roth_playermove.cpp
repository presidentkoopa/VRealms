//
// THE ORIGINAL'S PLAYER MOVEMENT, engine-free. See roth_playermove.h for the sources and
// docs/PLAYER_MOVEMENT_PORT.md for the checklist. Every block below names the ROTH.C lines it
// is transcribed from (R = renderer.c, PL = player.c, CP = collision_physics.c, D = doors.c,
// ML = map_load.c). The arithmetic keeps the original's widths: 16-bit words where it used
// words, the Watcom shl-16 and cwde idioms where they decide a bit.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_playermove.h"
#include <string.h>

namespace roth
{

const uint8_t kMoveKeyScancode[MK_COUNT] = { 0x50, 0x48, 0x4d, 0x4b, 0x33, 0x34, 0x51, 0x49, 0x47, 0x4f, 0x1e, 0x2c };

// The data image's tables, as the running original holds them (tools/move_test "TAB" lines,
// compared on every run).
const MoveTables &MoveTables::Original()
{
	static const MoveTables t = {
		{ 0, 9, 8, 7, 7, 6, 6, 5, 4, 4, 4, 3, 3, 2, 2, 1, 1, 1, 0, 0, 0 },
		{ 0xffff, 0x0000, 0x0180, 0x01c0, 0x0100, 0xffff, 0x0140, 0xffff, 0x0080, 0x0040, 0xffff, 0xffff, 0x00c0, 0xffff, 0xffff, 0xffff },
		{ 0, -2, -4, -6, -8, -10, -12, -14, -16, -18, -20, -21, -22, -20, -18, -16, -14, -11, -6, 0, 6, 11, 13, 15, 16, 14, 12, 10, 8, 6, 4, 2 },
	};
	return t;
}

namespace
{
inline uint16_t RW(const std::vector<uint8_t> &b, uint32_t o) { return (o + 1 < b.size()) ? (uint16_t)(b[o] | (b[o + 1] << 8)) : 0; }
inline void WW(std::vector<uint8_t> &b, uint32_t o, uint16_t v) { if (o + 1 < b.size()) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); } }
inline int16_t FW(const uint8_t *f, int o) { int16_t v; memcpy(&v, f + o, 2); return v; }
inline void SFW(uint8_t *f, int o, int16_t v) { memcpy(f + o, &v, 2); }
inline int32_t FD(const uint8_t *f, int o) { int32_t v; memcpy(&v, f + o, 4); return v; }
inline void SFD(uint8_t *f, int o, int32_t v) { memcpy(f + o, &v, 4); }
// dec cl; jg (a byte counter whose loop continues while ZF == 0 and SF == OF)
inline bool DecJg8(uint8_t &cl) { const uint8_t prev = cl; cl = (uint8_t)(cl - 1); const bool zf = cl == 0, sf = (cl & 0x80) != 0, of = prev == 0x80; return !(zf || sf != of); }
// approach_value (R:9911-9916)
inline int32_t Approach(int32_t eax, int32_t edx, int32_t ebx)
{
	if (eax == edx) return edx;
	if (eax < edx) { eax += ebx; return (eax <= edx) ? eax : edx; }
	eax -= ebx; return (eax >= edx) ? eax : edx;
}
inline int32_t DiSx(uint32_t edi) { return (int32_t)(int16_t)(uint16_t)edi; }
}

uint8_t  PlayerMove::E8(uint32_t o) const { o &= 0xffff; return o < es.size() ? es[o] : 0; }
uint16_t PlayerMove::E16(uint32_t o) const { return RW(es, o & 0xffff); }
int16_t  PlayerMove::VX(uint16_t off) const { return (int16_t)RW(gs, (uint16_t)(off + 8)); }
int16_t  PlayerMove::VY(uint16_t off) const { return (int16_t)RW(gs, (uint16_t)(off + 0xa)); }

//==========================================================================
//
// The image (ML:420-497, R:10267-10280, R:1637-1648, ML:133-147)
//
//==========================================================================

bool PlayerMove::BuildImage(const uint8_t *raw, size_t size)
{
	es.clear(); gs.clear(); ob.clear();
	if (raw == nullptr || size < 0x1c) return false;
	auto H = [&](int o) { return (uint32_t)(raw[o] | (raw[o + 1] << 8)); };
	if (H(0xe) != 0x5257 || H(2) != 0x70) return false;               // ML:431-433
	const uint32_t sec0 = H(0), vsz = H(0x14), cmdsz = H(0x1a), s7 = H(0x12), osz = H(0x16);
	// the file is read in order: sections 0 and 1, the command section, section 7, then the objects
	// (ML:452-479): each is the file's bytes at the running position.
	const size_t vStart = sec0, cStart = vStart + vsz, s7Start = cStart + cmdsz, oStart = s7Start + s7;
	if (oStart + osz > size) return false;
	es.assign(raw, raw + sec0);
	gs.assign(raw + vStart, raw + vStart + vsz);
	ob.assign(raw + oStart, raw + oStart + osz);
	// fixup_raw_sectors_after_load (R:10267): a 0xfffd door is closed, its ceiling saved at +0x10.
	{
		uint32_t esi = E16(4);
		int32_t ecx = E16(esi - 2);
		do {
			if ((int16_t)E16(esi + 0x14) == -3) { WW(es, esi + 0x10, E16(esi)); WW(es, esi, E16(esi + 2)); }
			esi += 0x1a;
		} while (--ecx > 0);
	}
	// mark_geom_sentinel_entries (R:1637): a solid face's flag byte +0xa gets 0x20.
	{
		uint32_t ebx = E16(6);
		uint32_t count = E16((ebx - 2) & 0xffff);
		if (count != 0) do {
			if (E16(ebx + 8) == 0xffff) es[(ebx + 0xa) & 0xffff] |= 0x20;
			ebx += 0xc;
		} while (--count != 0);
	}
	// flag_sectors_with_objects (ML:133-147): byte +0x16 bit 1 when the sector has an object list.
	{
		uint32_t off = E16(4);
		int32_t cnt = E16(off - 2);
		uint32_t list = 2;
		if (cnt != 0) do {
			if (RW(ob, list) != 0) es[(off + 0x16) & 0xffff] |= 2;
			off += 0x1a; list += 2;
		} while (--cnt > 0);
	}
	return true;
}

int PlayerMove::SectorCount() const { return es.size() > 8 ? (int)E16(E16(4) - 2) : 0; }
uint16_t PlayerMove::SectorOffset(int index) const { return (uint16_t)(E16(4) + index * 0x1a); }
int PlayerMove::SectorIndex(uint16_t off) const
{
	const uint16_t first = E16(4);
	if (off < first || ((off - first) % 0x1a) != 0) return -1;
	const int i = (off - first) / 0x1a;
	return i < SectorCount() ? i : -1;
}
void PlayerMove::SetSectorHeights(int index, int16_t ceiling, int16_t floor)
{
	if (index < 0 || index >= SectorCount()) return;
	const uint32_t s = SectorOffset(index);
	WW(es, s, (uint16_t)ceiling);
	WW(es, s + 2, (uint16_t)floor);
}
void PlayerMove::SetPlatformHeights(int index, int16_t undersideZ, int16_t topZ)
{
	if (index < 0 || index >= SectorCount()) return;
	const uint16_t l = E16(SectorOffset(index) + 0x18);
	if (l == 0) return;
	WW(es, l + 2, (uint16_t)undersideZ);
	WW(es, l + 8, (uint16_t)topZ);
}
void PlayerMove::SetSectorTextureOverride(int index, int8_t v)
{
	if (index < 0 || index >= SectorCount()) return;
	es[(SectorOffset(index) + 0xc) & 0xffff] = (uint8_t)v;
}
void PlayerMove::SetSectorDoorFlag(int index, bool on)
{
	if (index < 0 || index >= SectorCount()) return;
	uint8_t &b = es[(SectorOffset(index) + 0x16) & 0xffff];
	b = on ? (uint8_t)(b | 1) : (uint8_t)(b & ~1);
}

// init_movement_tuning_from_first_map (ML:197-209), the movement half. The position and angle it
// also copies are the host's to place.
void PlayerMove::InitTuning()
{
	const uint32_t off = E16(0xa);
	moveSpeed = E16(off + 8);
	const uint32_t v = ((uint32_t)E16(off + 0xa) * 2u) & 0xffffu;
	restHeight = (int32_t)v; height = (uint16_t)v; camHeight = (uint16_t)v;
	maxClimb = (uint16_t)(((uint32_t)E16(off + 0xc) * 2u + 1u) & 0xffffu);
	minFit = (uint16_t)(((uint32_t)E16(off + 0xe) * 2u) & 0xffffu);
}

//==========================================================================
//
// The held-key handlers (R:192-315, R:241, R:513-523, R:592-605)
//
//==========================================================================

void PlayerMove::KeyStrafeRight() { inputBits |= 0x0002; }
void PlayerMove::KeyStrafeLeft()  { inputBits |= 0x0008; }
void PlayerMove::KeyForward()
{
	if (dialogueBusy != 0 && freezeGate == 0x6ffff) return;      // the dialogue freeze (R:197-199)
	inputBits |= 0x0001;
}
void PlayerMove::KeyBackward()
{
	if (dialogueBusy != 0 && freezeGate == 0x6ffff) return;
	inputBits |= 0x0004;
}
void PlayerMove::KeyPitchUp()   { viewPitch += 2; pitchApplied = viewPitch; }
void PlayerMove::KeyPitchDown() { viewPitch -= 2; pitchApplied = viewPitch; }
void PlayerMove::KeyPitchCentreDown()
{
	if (viewPitch != 0) { pitchApplied = viewPitch; viewPitch = 0; }
	pitchApplied += -8;
}
void PlayerMove::KeyPitchCentreUp()
{
	if (viewPitch != 0) { pitchApplied = viewPitch; viewPitch = 0; }
	pitchApplied += 8;
}
void PlayerMove::KeyCrouch() { locoFlags |= 0x24; }
void PlayerMove::KeyTurnLeft()
{
	const int16_t max = runActive != 0 ? 0x0d : 0x08;
	const int16_t acc = (int16_t)turnAccum;
	if (acc < 0)         turnAccum = 2;
	else if (acc >= max) turnAccum = (uint16_t)max;
	else                 turnAccum = (uint16_t)(acc + 1);
	inputBits |= 0x10;
}
void PlayerMove::KeyTurnRight()
{
	const int16_t max = runActive != 0 ? (int16_t)-0x0d : (int16_t)-0x08;
	const int16_t acc = (int16_t)turnAccum;
	if (acc >= 0)        turnAccum = (uint16_t)(int16_t)-2;
	else if (acc <= max) turnAccum = (uint16_t)max;
	else                 turnAccum = (uint16_t)(acc - 1);
	inputBits |= 0x10;
}
void PlayerMove::KeyJump()
{
	locoFlags |= 0x01;
	if (jumpPhase != 0) return;
	const uint32_t s = (uint32_t)fallState;
	bool accept;
	if (s < 1) accept = true;
	else if (s <= 1) { if (fallAccum >= 5) return; accept = true; }
	else accept = (s == 2);
	if (accept) locoFlags |= 0x10;
}
void PlayerMove::ResetLocomotion()
{
	locoFlags = 0; jumpPhase = 0; fallState = 0; crouchMode = 0; fallAccum = 0;
}
void PlayerMove::ResetQueues()
{
	queueB.count = 0; queueA.count = 0; velY = 0; velX = 0;
}

// commit_player_position_delta (R:228-233) behind carry_player_span (R:969-978): the low word of
// each position moves, with no carry into the integer part's neighbour.
void PlayerMove::CarryPlayerSpan(uint16_t heightNow, int32_t dx, int32_t dy, int32_t dz)
{
	uint16_t ax = heightNow;
	uint16_t dxw = (uint16_t)((uint32_t)heightNow - (uint32_t)dz);
	if (!((int16_t)dxw > (int16_t)ax)) { uint16_t t = dxw; dxw = ax; ax = t; }
	const uint16_t pz = (uint16_t)(z >> 16);
	if ((int16_t)pz < (int16_t)ax || (int16_t)pz > (int16_t)dxw) return;
	// the words at 0x90a8e / 0x90a96 / 0x90a92 are the HIGH halves of the 16.16 dwords
	x = (x & 0xffffu) | ((uint32_t)(uint16_t)((x >> 16) + (uint16_t)dx) << 16);
	y = (y & 0xffffu) | ((uint32_t)(uint16_t)((y >> 16) + (uint16_t)dy) << 16);
	z = (z & 0xffffu) | ((uint32_t)(uint16_t)((z >> 16) + (uint16_t)dz) << 16);
}

//==========================================================================
//
// player_movement_tick (R:1044-1103) and apply_player_movement_input (R:1150-1231)
//
//==========================================================================

void PlayerMove::Tick(const MoveInput &in)
{
	// poll_mouse_input and mouse_edge_latch (R:1046, :1074) are the cursor's, not the movement's.
	uint32_t rt = runToggle;
	if (in.shift && crouchMode != 0) rt = ~rt;                   // R:1048-1049 (0x819cd)
	runActive = (int32_t)(rt & 1);
	if (movementMode != 1) return;                               // R:1053

	rateCounter = (uint16_t)(rateCounter - 1);
	if ((int16_t)rateCounter < 0)                                // R:1055-1071
	{
		rateCounter = moveSpeed;
		const int32_t avx = velX < 0 ? -velX : velX, avy = velY < 0 ? -velY : velY;
		if (!(avx > 0x20)) velX = 0;
		if (!(avy > 0x20)) velY = 0;
		velX >>= 1;
		velY >>= 1;
	}

	inputBits = 0;
	// dispatch_held_key_actions (input.c:208-226): clear_819c0_bits, then the table in order.
	locoFlags &= 0xfa;
	for (int k = 0; k < MK_COUNT; k++)
	{
		if (!(in.held & (1u << k))) continue;
		switch (k)
		{
		case MK_BACK: KeyBackward(); break;
		case MK_FORWARD: KeyForward(); break;
		case MK_TURN_RIGHT: KeyTurnRight(); break;
		case MK_TURN_LEFT: KeyTurnLeft(); break;
		case MK_STRAFE_LEFT: KeyStrafeLeft(); break;
		case MK_STRAFE_RIGHT: KeyStrafeRight(); break;
		case MK_PITCH_CENTRE_DOWN: KeyPitchCentreDown(); break;
		case MK_PITCH_CENTRE_UP: KeyPitchCentreUp(); break;
		case MK_PITCH_UP: KeyPitchUp(); break;
		case MK_PITCH_DOWN: KeyPitchDown(); break;
		case MK_JUMP: KeyJump(); break;
		case MK_CROUCH: KeyCrouch(); break;
		}
	}

	// the mouse-steer path reads these
	mouseDxIn = in.mouseDx; mouseDyIn = in.mouseDy;
	ApplyMovementInput();

	const int32_t eax = velX, edx = velY;
	if ((eax | edx) != 0) Enqueue(eax, edx);                     // R:1084-1101
}

void PlayerMove::Enqueue(int32_t eax, int32_t edx)
{
	Queue &q = queueSelect ? queueB : queueA;                     // 0x90b42 when the select is set
	if ((int16_t)q.count >= 0x10) return;
	const uint32_t sh = speedShift;
	if (sh != 0) { eax >>= (sh & 0x1f); edx >>= (sh & 0x1f); }
	q.d[q.count][0] = eax; q.d[q.count][1] = edx; q.count++;
	if ((inputBits & 0x80) || runActive != 0)                    // run: enqueue the same again
	{
		Queue &q2 = queueSelect ? queueB : queueA;
		if (!((int16_t)q2.count >= 0x10)) { q2.d[q2.count][0] = eax; q2.d[q2.count][1] = edx; q2.count++; }
	}
}

bool PlayerMove::AddExtraDelta(int32_t dx, int32_t dy)
{
	Queue &q = queueSelect ? queueB : queueA;                     // the one the ticks are filling
	if ((int16_t)q.count >= 0x10) return false;
	q.d[q.count][0] = dx; q.d[q.count][1] = dy; q.count++;
	return true;
}

void PlayerMove::ApplyMovementInput()
{
	if (cursorType == 1 && mouseRelative != 0)
	{
		// the mouse-steer path (R:1152-1192)
		int32_t eax = mouseDxIn;
		mouseDxIn = 0;
		if (!(eax < 0x10)) eax = 0x10;
		if (!(eax > -0x10)) eax = -0x10;
		eax *= 2;                                                  // shl 1
		eax += mouseSmooth;
		mouseSmooth = eax;
		if (eax == 0) mouseLimit = 1;
		mouseLimit += 1;
		{
			const int32_t lim = mouseLimit;
			if (!(eax < lim)) eax = lim;
			if (!(eax > -lim)) eax = -lim;
		}
		mouseSmooth -= eax;
		eax >>= 1;
		angle = (uint16_t)(angle - (uint16_t)eax);
		if (mouseDyIn == 0) goto idle_decay;
		{
			int32_t dy = mouseDyIn;
			if (!(dy < 0x18)) dy = 0x18;
			if (!(dy > -0x18)) dy = -0x18;
			mouseDyIn = dy;
			int32_t ady = dy < 0 ? -dy : dy;
			ady >>= 1;
			speedAccum = ady;
			rateCounter = (uint16_t)(rateCounter - 1);
			const int32_t ecx = -mouseDyIn;
			uint32_t ebx = (uint32_t)angle;
			ebx = (uint32_t)(-(int32_t)ebx);
			ebx += ebx;
			const int32_t sinv = ActorSin(ebx);
			velX += (int32_t)((uint32_t)sinv * (uint32_t)ecx) >> 2;
			const uint8_t bh = (uint8_t)((ebx >> 8) + 1);
			ebx = (ebx & 0xffff00ffu) | ((uint32_t)bh << 8);
			const int32_t cosv = ActorSin(ebx);
			velY += (int32_t)((uint32_t)cosv * (uint32_t)ecx) >> 2;
		}
		goto turn_tail;
	}
	// the keyboard path (R:1194-1213)
	if (inputBits & 0xf)
	{
		if (!(speedAccum >= 0x10)) speedAccum += 1;
		rateCounter = (uint16_t)(rateCounter - 1);
		uint16_t bx = tables->dir[inputBits & 0xf];
		if (bx != 0xffff)
		{
			bx = (uint16_t)(bx + angle);
			bx = (uint16_t)(-(int32_t)bx);
			bx = (uint16_t)(bx + bx);
			velX += ActorSin(bx) * 6;
			bx = (uint16_t)(bx + 0x100);
			velY += ActorSin(bx) * 6;
		}
		goto turn_tail;
	}
idle_decay:
	if (speedAccum > 0) speedAccum -= 1;                          // R:1216
turn_tail:
	if (inputBits & 0x10)                                        // R:1223-1230
	{
		const int16_t t = (int16_t)((int16_t)turnAccum >> 1);
		angle = (uint16_t)(angle + (uint16_t)t);
		turnMirror = (uint16_t)(turnMirror + (uint16_t)t);
	}
	else turnAccum = 0;
}

//==========================================================================
//
// move_player_with_collision (CP:1677-1796)
//
//==========================================================================

void PlayerMove::Frame(int fts)
{
	uint8_t frame[0x40];
	memset(frame, 0, sizeof frame);
	const uint16_t before = sector;
	Collide(frame);
	if (sector != before) stats.sectorChange++;
	// update_player_view_bob (PL:340-383) through EBP = this frame
	ViewBob(frame, fts);
}

// update_view_transform_params (PL:68-98), the one part of it that writes game state: the view pitch
// clamped to +-0x7e, written back to its source (g_view_pitch, or the applied pitch when that is 0).
// update_player_movement runs it straight after move_player_with_collision (PL:410-425). The camera
// record it also fills is the renderer's.
void PlayerMove::ClampViewPitch()
{
	int32_t *pp = viewPitch != 0 ? &viewPitch : &pitchApplied;
	int32_t v = *pp;
	if (v >= 0x7e)  { v = 0x7e;  *pp = v; }
	if (v <= -0x7e) { v = -0x7e; *pp = v; }
}

void PlayerMove::Collide(uint8_t *frame)
{
	queueSelect = (uint16_t)~queueSelect;                        // CP:1684
	Queue &q = queueSelect != 0 ? queueA : queueB;               // the queue the ticks were filling

	boxMax = 0x1c; boxMin = (int32_t)0xffffffe4u;               // CP:1688-1693
	radiusSq = 0xc400; radiusSq2 = 0xc710; cornerSq = 0xc440; radius = 0x70;
	span = minFit;
	portalContinue = 0;
	stepActive = 0;
	stackCount = 0;
	{
		uint16_t climb = maxClimb;                                // CP:1698-1702
		if (crouchMode != 0) climb = 0x10;
		maxStep = climb;
	}
	SFW(frame, 0x1e, (int16_t)(uint16_t)(z >> 16));             // CP:1705
	SFD(frame, 0x24, 0x100000);
	flagMask = 0x81;

	restoreX = savedX = qX = x;                                  // CP:1709-1713
	savedZ = qZ = z;
	restoreY = savedY = qY = y;

	uint16_t count = q.count;
	int32_t cx;
	bool empty;
	int idx = 0;
	if (count == 0) { cx = 1; empty = true; savedSector = sector; }
	else { cx = count; empty = false; q.count = 0; }

	for (;;)
	{
		if (!empty)
		{
			savedSector = sector;
			restoreX = savedX = qX;
			savedZ = qZ;
			restoreY = savedY = qY;
			const int32_t dX = q.d[idx][0], dY = q.d[idx][1];
			qX += (uint32_t)dX; qY += (uint32_t)dY;
			const int32_t bx = dX >> 10, by = dY >> 10;
			int32_t sq = (int32_t)((uint32_t)(bx * bx) + (uint32_t)(by * by));
			if (!((uint32_t)sq > 0x100000u)) sq = 0x100000;
			SFD(frame, 0x24, sq);
			fireGate = 1;
		}
		empty = false;
		// collide_substep_track_sector (CP:1486-1495)
		if (!FindSectorAndCollide(frame))
		{
			if (sector != 0 && sectorCache != sector) sectorCache = sector;
		}
		idx++;
		cx = (int16_t)(cx - 1);
		if (!(cx > 0)) break;
	}

	uint16_t ax = 0;
	bool store = false;
	if (sector != 0)                                             // CP:1749-1765
	{
		uint32_t fq = 0;
		FindQuerySector(sector, frame, &fq);
		ax = (uint16_t)fq;
		if (ax != 0) store = true;
	}
	if (!store)
	{
		ax = savedSector;
		if (ax != 0) { qX = savedX; qZ = savedZ; qY = savedY; }
	}
	sector = ax;

	if (ax != 0)                                                 // CP:1767-1781
	{
		uint8_t al = E8(ax + 0xb);
		if (al != 0) al = (uint8_t)(al + lightOffset);
		viewShade = al;
		if (E8(ax + 0xa) & 2) { sectorCacheFlag = 0; viewShade = (uint8_t)(viewShade - 0x10); }
		else sectorCacheFlag = 0xfff3;
	}
	else viewShade = (uint8_t)(viewShade - 0x10);

	x = qX; z = qZ; y = qY;                                      // CP:1784-1786
}

// update_player_view_bob (PL:340-383)
void PlayerMove::ViewBob(const uint8_t *frame, int fts)
{
	int32_t cnt = fts;
	if (cnt == 0) return;
	const uint16_t floor_w = (uint16_t)FW(frame, 0), ceil_w = (uint16_t)FW(frame, 2);
	const int32_t floorZ = (int16_t)floor_w, ceilZ = (int16_t)ceil_w;
	if (((floor_w >> 8) & 0xff) == 0x80) return;
	const int32_t esi = -viewClearance;
	int32_t ebx = (int16_t)(uint16_t)(z >> 16);
	do
	{
		ebx = (int32_t)VerticalPhysics((uint32_t)floorZ, (uint32_t)ceilZ, (uint32_t)ebx);
		int32_t diff = pitchApplied - esi;
		if (diff != 0)
		{
			if (diff < 0) { if (diff < -3) diff = -3; }
			else { if (diff >= 3) diff = 3; }
			pitchApplied -= diff;
		}
	} while (--cnt > 0);

	z = (z & 0xffffu) | ((uint32_t)(uint16_t)ebx << 16);          // the word at 0x90a92
	int32_t si = (int16_t)height;
	const uint32_t phase = (uint32_t)fts + bobPhase;
	bobPhase = phase;
	if (speedAccum != 0)
	{
		const uint32_t i = ((phase * 3u) >> 1) & 0x3e;
		int16_t c = tables->bob[i >> 1];
		c = (int16_t)(c * (int16_t)speedAccum);
		c = (int16_t)(c >> 4);
		bobOffset = c;
		si = (int16_t)(si + c);
		si = (int16_t)(si + (int16_t)ebx);
		const int16_t lo = (int16_t)(floorZ + 0x18), hi = (int16_t)(ceilZ - 0x18);
		if ((int16_t)si <= lo) si = lo;
		if ((int16_t)si >= hi) si = hi;
		si = (int16_t)(si - (int16_t)ebx);
	}
	camHeight = (uint16_t)si;
}

// update_player_vertical_physics (PL:205-329)
uint32_t PlayerMove::VerticalPhysics(uint32_t eax_in, uint32_t edx_in, uint32_t ebx_in)
{
	const int32_t floor_z = (int32_t)eax_in;
	int32_t pos = (int32_t)ebx_in;
	const uint8_t mode = movementMode;
	if (mode != 0x20 && (mode & 1) == 0) return ebx_in;

	const int32_t ceil_lim = (int32_t)edx_in - 0x1c;
	uint32_t edi = height;

	if (health <= 0) KeyCrouch();                                // PL:218-219
	if (ceil_lim - floor_z < 0x70) locoFlags |= 0x24;           // PL:221-222

	bool can_jump = false;
	if (fallState == 0) can_jump = true;
	else if (fallState == 1) can_jump = fallAccum < 5;
	else if (fallState == 2) can_jump = true;

	if (can_jump && (locoFlags & 0x10))
	{
		locoFlags -= 0x10;
		if (crouchMode == 0 && pos < ceil_lim && jumpPhase == 0 && fallState == 0)
			jumpPhase = 1;
	}

	if (jumpPhase == 0)
	{
		if (locoFlags & 4)
		{
			crouchMode = 1;
			pos = Approach(pos, floor_z, 4);
			edi = (uint32_t)Approach(DiSx(edi), 0x30, 6);
		}
		else crouchMode = 0;
	}

	const int32_t phase = jumpPhase;
	if (phase != 0)
	{
		pos += tables->jump[phase];
		const int32_t apex = pos + DiSx(edi);
		if (apex > ceil_lim)
		{
			pos = ceil_lim;
			jumpPhase = 0; fallState = -1; landPending = 1;
			pos -= DiSx(edi);
		}
		else if (phase >= 0x13) { jumpPhase = 0; fallState = -1; landPending = 1; }
		else jumpPhase = phase + 1;
	}

	if (pos > floor_z + 2 && jumpPhase == 0)
	{
		landPending = 1;
		fallState = (pos <= floor_z + 0xd) ? 1 : 2;
	}

	if (fallState != 0)
	{
		int32_t step = pos - floor_z;
		locoFlags &= 0xeb;
		if (step > 0)
		{
			if (step > fallAccum) step = fallAccum;
			pos -= step;
			fallAccum++;
			if (pos <= floor_z) { edi = (uint32_t)Approach(DiSx(edi), 0x30, step); pos = floor_z; }
		}
		else
		{
			if (landPending != 0)
			{
				landPending = 0;
				if (fallAccum > 0x16)
				{
					int32_t dmg = fallAccum - 0x16;
					dmg = dmg * dmg;
					if (dmg > 0x1f40) dmg = 0x1f40;
					if (host != nullptr) host->DamageDirect((uint32_t)dmg);
				}
				if (fallAccum > 0xe && landingSound != 0 && host != nullptr)
					host->LandingSound((uint32_t)landingSound - 1);
			}
			int32_t v = fallAccum;
			if (v > 8) v -= 4;
			v--;
			if (v == 0) fallState = 0;
			pos -= v;
			if (pos < floor_z) { edi = (uint32_t)Approach(DiSx(edi), 0x30, v); pos = floor_z; }
			fallAccum = v;
		}
	}

	if ((fallState | jumpPhase | crouchMode) == 0)
	{
		pos = Approach(pos, floor_z, 4);
		edi = (uint32_t)Approach(DiSx(edi), restHeight, 4);
	}

	if (DiSx(edi) + pos < floor_z) pos = floor_z;
	if (pos > ceil_lim) pos = ceil_lim;
	if (DiSx(edi) + pos > ceil_lim)
	{
		edi = (uint32_t)(ceil_lim - pos);
		if (DiSx(edi) < 0) { edi = 0; if (pos < floor_z) pos = floor_z; }
	}
	height = (uint16_t)edi;
	return (uint32_t)pos;
}

//==========================================================================
//
// The sector search (CP:91-162, CP:906-1028)
//
//==========================================================================

int PlayerMove::ScanSectorEdgesAtY(uint16_t sec, uint8_t *frame)
{
	uint8_t cl = E8(sec + 0xd);
	uint16_t si = E16(sec + 0xe);
	const int16_t qx = (int16_t)(qX >> 16), qy = (int16_t)(qY >> 16);
	int16_t maxX = (int16_t)0x8000, minX = 0x7fff;
	for (;;)
	{
		const uint16_t vB = E16(si + 2), vA = E16(si);
		const int16_t bY = VY(vB), aY = VY(vA);
		const int16_t da = (int16_t)(aY - qy);
		bool crossed = false;
		int16_t crossX = 0;
		if (da == 0) { crossX = VX(vA); crossed = true; }
		else if (da > 0)
		{
			const int16_t db = (int16_t)(bY - qy);
			if (db < 0)
			{
				const int16_t yspan = (int16_t)(da - db), num = (int16_t)(qy - bY), xspan = (int16_t)(VX(vA) - VX(vB));
				const int16_t qv = (int16_t)(((int32_t)num * xspan) / yspan);
				crossX = (int16_t)(qv + VX(vB)); crossed = true;
			}
		}
		else
		{
			const int16_t db = (int16_t)(bY - qy);
			if (db > 0)
			{
				const int16_t yspan = (int16_t)(db - da), num = (int16_t)(qy - aY), xspan = (int16_t)(VX(vB) - VX(vA));
				const int16_t qv = (int16_t)(((int32_t)num * xspan) / yspan);
				crossX = (int16_t)(qv + VX(vA)); crossed = true;
			}
		}
		if (crossed)
		{
			if (crossX >= maxX) maxX = crossX;
			if (crossX <= minX) minX = crossX;
		}
		si = (uint16_t)(si + 0xc);
		if (!DecJg8(cl)) break;
	}
	SFW(frame, 4, maxX); SFW(frame, 6, minX);
	if (minX > qx) return 1;
	if (maxX < qx) return 1;
	return 0;
}

int PlayerMove::SearchFromHint(uint32_t hint, uint8_t *frame, uint32_t *out)
{
	const uint32_t s = hint & 0xffff;
	*out = s;
	if (s == 0) return 1;
	return ScanSectorEdgesAtY((uint16_t)s, frame);
}

int PlayerMove::SearchNeighbours(uint32_t secIn, uint8_t *frame, uint32_t *out)
{
	const uint32_t s = secIn & 0xffff;
	if (s == 0) { *out = 0; return 1; }
	uint8_t cl = E8(s + 0xd);
	uint16_t edi = E16(s + 0xe);
	for (;;)
	{
		const uint16_t link = E16(edi + 8);
		if (link != 0xffff)
		{
			const uint16_t nb = E16(link + 6);
			if (ScanSectorEdgesAtY(nb, frame) == 0) { *out = nb; return 0; }
		}
		edi = (uint16_t)(edi + 0xc);
		if (!DecJg8(cl)) break;
	}
	*out = 0;
	return 1;
}

int PlayerMove::SearchGlobal(uint8_t *frame, uint32_t *out)
{
	bestSector = 0;
	uint16_t esi = E16(4);
	uint16_t cx = E16((uint16_t)(esi - 2));
	if (cx != 0)
	{
		for (;;)
		{
			if (ScanSectorEdgesAtY(esi, frame) == 0)
			{
				bestSector = esi;
				const uint16_t bz = (uint16_t)(qZ >> 16), ceil = E16(esi), flr = E16(esi + 2);
				if (!(bz > ceil) && !(bz < flr)) { *out = esi; return 0; }
			}
			esi = (uint16_t)(esi + 0x1a);
			const uint16_t prev = cx; cx = (uint16_t)(cx - 1);
			const bool zf = cx == 0, sf = (cx & 0x8000) != 0, of = prev == 0x8000;
			if (zf || sf != of) break;
		}
	}
	*out = bestSector;
	return bestSector != 0 ? 0 : 1;
}

void PlayerMove::FindQuerySector(uint32_t hint, uint8_t *frame, uint32_t *out)
{
	uint32_t s = 0;
	if (SearchFromHint(hint, frame, &s) == 0) { *out = s; return; }
	if (SearchNeighbours(s, frame, &s) == 0) { *out = s; return; }
	if (SearchGlobal(frame, &s) == 0) { *out = s; return; }
	*out = 0;
}

uint16_t PlayerMove::Locate(int16_t px, int16_t py, int16_t pz, uint16_t hint)
{
	uint8_t scratch[0x40] = {};
	qX = (uint32_t)(uint16_t)px << 16; qY = (uint32_t)(uint16_t)py << 16; qZ = (uint32_t)(uint16_t)pz << 16;
	uint32_t s = 0;
	if (hint != 0)
	{
		if (SearchFromHint(hint, scratch, &s) == 0) return (uint16_t)s;
		if (SearchNeighbours(hint, scratch, &s) == 0) return (uint16_t)s;
	}
	SearchGlobal(scratch, &s);
	return (uint16_t)s;
}

//==========================================================================
//
// find_sector_and_collide (CP:1448-1478), resolve_collisions_in_sector (CP:1402-1434),
// collide_sector_walls (CP:1030-1068)
//
//==========================================================================

int PlayerMove::FindSectorAndCollide(uint8_t *frame)
{
	SFW(frame, 0, (int16_t)0x8000);
	SFW(frame, 2, 0x7fff);
	SFD(frame, 0x20, 6);
	uint32_t s = 0;
	const uint16_t cur = sector;
	if (cur == 0)
	{
		if (SearchGlobal(frame, &s)) return 1;
		sector = (uint16_t)s;
	}
	else
	{
		if (!SearchFromHint(cur, frame, &s)) { }
		else if (!SearchNeighbours(cur, frame, &s)) sector = (uint16_t)s;
		else if (!SearchGlobal(frame, &s)) sector = (uint16_t)s;
		else return 1;
	}
	ResolveInSector(s, frame);
	return 0;
}

void PlayerMove::ResolveInSector(uint32_t sec, uint8_t *frame)
{
	GatherNearby();                                              // CP:1405 gather_nearby_doors
	for (;;)
	{
		portalContinue = 0;
		CollideSectorWalls(sec, frame);
		// stepActive is 0 for the player (CP:1695): the POINT branch.
		if (entityCount != 0)
			if (ResolveObjects(frame)) goto restore;
		{
			const int32_t b = FD(frame, 0x20) - 1;
			SFD(frame, 0x20, b);
			if (b < 0) goto restore;
		}
		stepActive = 0;
		if (portalContinue != 0) continue;
		return;
	}
restore:
	qX = restoreX;
	qY = restoreY;
}

void PlayerMove::CollideSectorWalls(uint32_t sector_, uint8_t *frame)
{
	const uint16_t s = (uint16_t)sector_;
	const int16_t ceiling = (int16_t)E16(s), floor = (int16_t)E16(s + 2);
	SFW(frame, 2, ceiling);
	SFW(frame, 0, floor);
	const uint16_t lrec = E16(s + 0x18);
	if (lrec != 0)
	{
		const int16_t a = (int16_t)E16(lrec + 8), d = (int16_t)E16(lrec + 2);
		if (a >= d)
		{
			if ((int16_t)(ceiling - a) < (int16_t)span) SFW(frame, 2, d);
			else
			{
				const int16_t b = (int16_t)((int16_t)(a - (int16_t)(uint16_t)(qZ >> 16)) - (int16_t)height);
				if (b >= 0) SFW(frame, 2, d);
				else SFW(frame, 0, a);
			}
		}
	}
	stackCount = 0;
	CollidePointWalls(sector_, frame);                           // stepActive == 0 (the player is a POINT sweep)
}

//==========================================================================
//
// collide_point_walls_recursive (CP:674-900) and crw2_portal_narrow (CP:595-647)
//
//==========================================================================

bool PlayerMove::DoorRecordExists(uint16_t key) const
{
	// find_record_by_id (R:1394-1406): CF 0 = a door record keyed by this sector exists.
	for (const MoveDoor &d : doors) if (d.sector == key) return true;
	return false;
}

int PlayerMove::PortalNarrow(uint8_t *fp)
{
	const uint16_t nbr = (uint16_t)FW(fp, 0x1c);
	int16_t dx = (altZ != 0) ? (int16_t)altZ : (int16_t)E16(nbr + 2);
	int16_t ax = (int16_t)E16(nbr);
	if ((int16_t)(FW(fp, 0x1e) - ax) > (int16_t)span) return 0;
	int16_t b = (int16_t)(dx - FW(fp, 0x1e));
	if (b > (int16_t)maxStep) return 0;
	b = (int16_t)(b - (int16_t)height);
	if (b >= 0) return 0;
	if (!(dx > FW(fp, 0))) dx = FW(fp, 0);
	if (!(ax < FW(fp, 2))) ax = FW(fp, 2);
	if ((int16_t)(ax - dx) < (int16_t)span) return 0;

	const uint16_t lrec = E16(nbr + 0x18);
	if (lrec != 0)
	{
		const int16_t l8 = (int16_t)E16(lrec + 8), l2 = (int16_t)E16(lrec + 2);
		if (l8 < l2) goto recurse;
		if (portalKind == 2) goto from_top;
		{
			int16_t c = (int16_t)(ax - l8);
			if (c < (int16_t)span) goto from_bot;
			c = (int16_t)(l8 - FW(fp, 0x1e));
			if (c > (int16_t)maxStep) goto from_bot;
			c = (int16_t)(c - (int16_t)height);
			if (c >= 0) goto from_bot;
			if (l8 > dx) dx = l8;
			goto recurse;
		}
	from_bot:
		if (portalKind == 1) return 0;
	from_top:
		if ((int16_t)(l2 - dx) < (int16_t)span) return 0;
		if (l2 < ax) ax = l2;
	}
recurse:
	SFW(fp, 0, dx);
	SFW(fp, 2, ax);
	CollidePointWalls(nbr, fp);
	return 1;
}

void PlayerMove::CollidePointWalls(uint32_t sec, uint8_t *fp)
{
	const uint16_t cnt = stackCount;
	if ((int16_t)cnt >= 0x1e) return;
	const uint16_t si16 = (uint16_t)(sec & 0xffff);
	for (uint16_t k = 0; k < cnt; k++) if (stack[k] == si16) return;
	stack[cnt] = si16;
	stackCount = (uint16_t)(cnt + 1);

	uint8_t wcount = E8(si16 + 0xd);
	uint16_t wall = E16(si16 + 0xe);
	const uint8_t mask = flagMask;

	for (;;)
	{
		altZ = 0;
		const uint16_t vA = E16(wall), link = E16(wall + 8);
		if (link == 0xffff) SFW(fp, 0x1c, (int16_t)0xffff);
		else
		{
			portalKind = 0;
			const uint8_t dl = (uint8_t)(E8(wall + 0xa) & mask);
			bool direct = false;
			if (dl == 0) direct = true;
			else if (!(dl & 0x80)) SFW(fp, 0x1c, (int16_t)0xffff);
			else
			{
				const uint16_t rec = E16(wall + 4);
				const int8_t sc = (int8_t)E8(si16 + 0xc);
				if ((E8(rec + 8) & 8) && sc < 0)
				{
					altZ = (int32_t)(uint16_t)(((uint16_t)(uint8_t)(-sc) << 2) + (uint16_t)E16(si16 + 2));
					direct = true;
				}
				else
				{
					const uint16_t nbr = E16(link + 6);
					SFW(fp, 0x1c, (int16_t)nbr);
					if (E16(nbr + 0x18) != 0)
					{
						portalKind = 1;
						if ((dl & 3) == 0) portalKind = 2;
					}
				}
			}
			if (direct)
			{
				const uint16_t nbr = E16(link + 6);
				SFW(fp, 0x1c, (int16_t)nbr);
				if ((uint16_t)E16(nbr + 0x14) >= 0xfffe && !DoorRecordExists(nbr))
					SFW(fp, 0x1c, (int16_t)0xffff);
			}
		}

		{
			const uint16_t vB = E16(wall + 2);
			const int16_t qx = (int16_t)(qX >> 16), qy = (int16_t)(qY >> 16);
			const int16_t boxLo = (int16_t)boxMin, boxHi = (int16_t)boxMax;
			int16_t xa = (int16_t)(VX(vA) - qx), xb = (int16_t)(VX(vB) - qx);
			if (!(xa > xb)) { const int16_t t = xa; xa = xb; xb = t; }
			if (xa <= boxLo) goto next_wall;
			if (xb >= boxHi) goto next_wall;
			const uint16_t xspan = (uint16_t)(xa - xb);
			int16_t ya = (int16_t)(VY(vA) - qy), yb = (int16_t)(VY(vB) - qy);
			if (!(ya > yb)) { const int16_t t = ya; ya = yb; yb = t; }
			if (ya <= boxLo) goto next_wall;
			if (yb >= boxHi) goto next_wall;

			const bool caseHoriz = (ya == yb);
			const bool caseVert = !caseHoriz && xspan == 0;
			int32_t respX = 0, respY = 0, ecx = 0;

			if (!caseHoriz && !caseVert)
			{
				const int16_t Ay = VY(vA), By = VY(vB), Ax = VX(vA), Bx = VX(vB);
				const int32_t dyA8 = (int32_t)(int16_t)(Ay - By) * 8;   // shl 3
				SFW(fp, 0xc, (int16_t)dyA8);
				SFD(fp, 0xe, (int32_t)((uint32_t)dyA8 * (uint32_t)dyA8));
				ecx = FD(fp, 0xe);
				const int32_t dxA8 = (int32_t)(int16_t)(Ax - Bx) * 8;   // shl 3
				SFW(fp, 0x12, (int16_t)dxA8);
				SFD(fp, 0x14, (int32_t)((uint32_t)dxA8 * (uint32_t)dxA8));
				ecx += FD(fp, 0x14);
				const int32_t tY = (int32_t)qY >> 13;
				SFW(fp, 0x1a, (int16_t)(tY - ((int32_t)(uint16_t)Ay << 3)));
				const int32_t prod1 = (int32_t)(FW(fp, 0x1a) * FW(fp, 0x12));
				const int32_t tX = (int32_t)qX >> 13;
				SFW(fp, 0x18, (int16_t)(tX - ((int32_t)(uint16_t)Ax << 3)));
				const int32_t prod2 = (int32_t)(FW(fp, 0x18) * FW(fp, 0xc));
				const int32_t cross = prod2 - prod1;
				const int32_t perp2 = (int32_t)(((int64_t)cross * cross) / (int32_t)ecx);
				if (perp2 >= radiusSq) goto next_wall;
			}

			if (FW(fp, 0x1c) != (int16_t)0xffff)
				if (PortalNarrow(fp)) { stats.portalRecurse++; goto next_wall; }

			if (caseHoriz)
			{
				int16_t e = (int16_t)boxMax;
				if (!((int16_t)(VX(vA) - VX(vB)) < 0)) e = (int16_t)(-e);
				respY = (int32_t)((uint32_t)(uint16_t)(int16_t)(VY(vA) - e) << 16);
				respX = (int32_t)qX;
				segThreshold = 0x1d4c;
			}
			else if (caseVert)
			{
				int16_t e = (int16_t)boxMax;
				if ((int16_t)(VY(vA) - VY(vB)) < 0) e = (int16_t)(-e);
				respX = (int32_t)((uint32_t)(uint16_t)(int16_t)(VX(vA) - e) << 16);
				respY = (int32_t)qY;
				segThreshold = 0x1d4c;
			}
			else
			{
				const int16_t Ay = VY(vA), Ax = VX(vA);
				const int32_t t28 = (int32_t)(FW(fp, 0x12) * -(int32_t)FW(fp, 0x18)) + (int32_t)(FW(fp, 0xc) * -(int32_t)FW(fp, 0x1a));
				SFD(fp, 0x28, t28);
				const int32_t projX = (int32_t)(((int64_t)FW(fp, 0x12) * FD(fp, 0x28)) / (int32_t)ecx);
				const int32_t projY = (int32_t)(((int64_t)FW(fp, 0xc) * FD(fp, 0x28)) / (int32_t)ecx);
				SFD(fp, 0x30, (int32_t)((uint32_t)(((int32_t)Ay * 8) - projY) << 13));
				SFD(fp, 0x2c, (int32_t)((uint32_t)(((int32_t)Ax * 8) - projX) << 13));
				int32_t s = (int32_t)(((int64_t)radiusSq2 * FD(fp, 0x14)) / (int32_t)ecx);
				s = ActorIsqrt((uint32_t)s);
				if (!((uint16_t)FW(fp, 0x12) & 0x8000)) s = -s;
				s = (int32_t)(int16_t)s; s = (int32_t)((uint32_t)s << 13);
				SFD(fp, 0x30, FD(fp, 0x30) - s);
				s = (int32_t)(((int64_t)radiusSq2 * FD(fp, 0xe)) / (int32_t)ecx);
				s = ActorIsqrt((uint32_t)s);
				if ((uint16_t)FW(fp, 0xc) & 0x8000) s = -s;
				s = (int32_t)(int16_t)s; s = (int32_t)((uint32_t)s << 13);
				SFD(fp, 0x2c, FD(fp, 0x2c) - s);
				respX = FD(fp, 0x2c);
				respY = FD(fp, 0x30);
				segThreshold = (int32_t)0xe09c;
			}

			{
				hitFlags |= 1;
				// the installed point-collision callback 0x8c178 (CP:649-660, :789-790) is the query
				// context; the player's is 0 (CP:1735 passes g_player_query = EDX = 0), so it never fires.
				int32_t cMaxX = (int32_t)((uint32_t)(uint16_t)VX(vB) << 16), cMinX = (int32_t)((uint32_t)(uint16_t)VX(vA) << 16);
				if (!(cMaxX > cMinX)) { const int32_t t = cMaxX; cMaxX = cMinX; cMinX = t; }
				bool inSeg = true;
				if (cMinX > respX) inSeg = false;
				if (cMaxX < respX) inSeg = false;
				if (inSeg)
				{
					int32_t cMaxY = (int32_t)((uint32_t)(uint16_t)VY(vB) << 16), cMinY = (int32_t)((uint32_t)(uint16_t)VY(vA) << 16);
					if (!(cMaxY > cMinY)) { const int32_t t = cMaxY; cMaxY = cMinY; cMinY = t; }
					if (cMinY > respY) inSeg = false;
					if (cMaxY < respY) inSeg = false;
				}
				const int32_t ddx = (respX - (int32_t)restoreX) >> 10, ddy = (respY - (int32_t)restoreY) >> 10;
				int32_t dist2 = (int32_t)((uint32_t)(ddx * ddx) + (uint32_t)(ddy * ddy));
				if (!inSeg)
				{
					const int32_t adj = dist2 - segThreshold;
					if (adj >= 0 && adj > FD(fp, 0x24))
					{
						const int32_t ex = (int32_t)(int16_t)(((int32_t)qX >> 13) - ((int32_t)(uint16_t)VX(vA) << 3));
						const int32_t ey = (int32_t)(int16_t)(((int32_t)qY >> 13) - ((int32_t)(uint16_t)VY(vA) << 3));
						if (ex * ex + ey * ey >= cornerSq) goto next_wall;
						const uint32_t txi = qX >> 15, tyi = qY >> 15;
						const int16_t a2x = (int16_t)((uint16_t)VX(vA) + (uint16_t)VX(vA));
						const int16_t a2y = (int16_t)((uint16_t)VY(vA) + (uint16_t)VY(vA));
						const uint32_t bearing = ActorBearing(a2x, (int16_t)tyi, a2y, (int16_t)txi);
						uint32_t bi = (bearing + bearing) & 0x3fe;
						int32_t tbl = ActorSin(bi);
						qX = (uint32_t)((int32_t)((uint32_t)(uint16_t)VX(vA) << 16) + tbl * radius);
						const uint32_t bh = ((bi >> 8) + 1) & 3;
						bi = (bi & 0x00ff) | (bh << 8);
						tbl = ActorSin(bi);
						qY = (uint32_t)((int32_t)((uint32_t)(uint16_t)VY(vA) << 16) + tbl * radius);
						portalContinue = 0xffff;
						stats.wallSlide++;
						goto next_wall;
					}
				}
				qX = (uint32_t)respX;
				qY = (uint32_t)respY;
				if (!((uint32_t)dist2 > 0x100000u)) dist2 = 0x100000;
				SFD(fp, 0x24, dist2);
				portalContinue = 0xffff;
				stats.wallPush++;
			}
		}
	next_wall:
		wall = (uint16_t)(wall + 0xc);
		wcount = (uint8_t)(wcount - 1);
		if (!((int8_t)wcount > 0)) break;
	}
}

//==========================================================================
//
// clip_query_circle_to_edge (CP:210-371) and clip_locate_query_to_object (CP:373-398):
// a door's leaf, edge by edge.
//
//==========================================================================

int PlayerMove::ClipCircleToEdge(uint8_t *fp)
{
	const int16_t Ax = edgeAx, Ay = edgeAy, Bx = edgeBx, By = edgeBy;
	const int16_t qx = (int16_t)(qX >> 16), qy = (int16_t)(qY >> 16);
	const int16_t boxLo = (int16_t)boxMin, boxHi = (int16_t)boxMax;

	int16_t ax = (int16_t)(Ax - qx);
	int16_t dx = (int16_t)(-(int16_t)(qx - Bx));
	if (!(ax > dx)) { const int16_t t = ax; ax = dx; dx = t; }
	if (ax <= boxLo) return 0;
	if (dx >= boxHi) return 0;
	const int16_t xspan = (int16_t)(ax - dx);
	ax = (int16_t)(Ay - qy);
	dx = (int16_t)(By - qy);
	if (!(ax > dx)) { const int16_t t = ax; ax = dx; dx = t; }
	if (ax <= boxLo) return 0;
	if (dx >= boxHi) return 0;
	const bool yspanZero = (ax == dx);

	int32_t respX, respY;
	if (yspanZero)
	{
		int32_t e = boxHi;
		if ((int16_t)(Ax - Bx) >= 0) e = -e;
		e = (int16_t)(e - Ay);
		e = -e;
		respY = (int32_t)((uint32_t)(uint16_t)e << 16);
		respX = (int32_t)qX;
	}
	else if (xspan == 0)
	{
		int32_t e = boxHi;
		if ((int16_t)(Ay - By) < 0) e = -e;
		e = (int16_t)(Ax - e);
		respX = (int32_t)((uint32_t)(uint16_t)e << 16);
		respY = (int32_t)qY;
	}
	else
	{
		const int32_t dyA8 = (int32_t)(int16_t)(Ay - By) * 8;   // shl 3
		SFW(fp, 0xc, (int16_t)dyA8);
		SFD(fp, 0xe, (int32_t)((uint32_t)dyA8 * (uint32_t)dyA8));
		int32_t ecx = FD(fp, 0xe);
		const int32_t dxA8 = (int32_t)(int16_t)(Ax - Bx) * 8;   // shl 3
		SFW(fp, 0x12, (int16_t)dxA8);
		SFD(fp, 0x14, (int32_t)((uint32_t)dxA8 * (uint32_t)dxA8));
		ecx += FD(fp, 0x14);
		const int32_t tY = (int32_t)qY >> 13;
		SFW(fp, 0x1a, (int16_t)(tY - ((int32_t)(uint16_t)Ay << 3)));
		const int32_t prod1 = (int32_t)(FW(fp, 0x1a) * FW(fp, 0x12));
		const int32_t tX = (int32_t)qX >> 13;
		SFW(fp, 0x18, (int16_t)(tX - ((int32_t)(uint16_t)Ax << 3)));
		const int32_t prod2 = (int32_t)(FW(fp, 0x18) * FW(fp, 0xc));
		const int32_t cross = prod2 - prod1;
		const int32_t perp2 = (int32_t)(((int64_t)cross * cross) / (int32_t)ecx);
		if (perp2 >= radiusSq) return 0;
		const int32_t t28 = (int32_t)(FW(fp, 0x12) * -(int32_t)FW(fp, 0x18)) + (int32_t)(FW(fp, 0xc) * -(int32_t)FW(fp, 0x1a));
		SFD(fp, 0x28, t28);
		const int32_t projX = (int32_t)(((int64_t)FW(fp, 0x12) * FD(fp, 0x28)) / (int32_t)ecx);
		const int32_t projY = (int32_t)(((int64_t)FW(fp, 0xc) * FD(fp, 0x28)) / (int32_t)ecx);
		SFD(fp, 0x30, (int32_t)((uint32_t)(((int32_t)(int16_t)Ay * 8) - projY) << 13));
		SFD(fp, 0x2c, (int32_t)((uint32_t)(((int32_t)(int16_t)Ax * 8) - projX) << 13));
		int32_t s = (int32_t)(((int64_t)radiusSq2 * FD(fp, 0x14)) / (int32_t)ecx);
		s = ActorIsqrt((uint32_t)s);
		if (!((uint16_t)FW(fp, 0x12) & 0x8000)) s = -s;
		s = (int32_t)(int16_t)s; s = (int32_t)((uint32_t)s << 13);
		SFD(fp, 0x30, FD(fp, 0x30) - s);
		s = (int32_t)(((int64_t)radiusSq2 * FD(fp, 0xe)) / (int32_t)ecx);
		s = ActorIsqrt((uint32_t)s);
		if ((uint16_t)FW(fp, 0xc) & 0x8000) s = -s;
		s = (int32_t)(int16_t)s; s = (int32_t)((uint32_t)s << 13);
		SFD(fp, 0x2c, FD(fp, 0x2c) - s);
		respX = FD(fp, 0x2c);
		respY = FD(fp, 0x30);
	}

	int32_t cMaxX = (int32_t)((uint32_t)(uint16_t)Bx << 16), cMinX = (int32_t)((uint32_t)(uint16_t)Ax << 16);
	if (!(cMaxX > cMinX)) { const int32_t t = cMaxX; cMaxX = cMinX; cMinX = t; }
	bool inSeg = true;
	if (cMinX > respX) inSeg = false;
	if (cMaxX < respX) inSeg = false;
	if (inSeg)
	{
		int32_t cMaxY = (int32_t)((uint32_t)(uint16_t)By << 16), cMinY = (int32_t)((uint32_t)(uint16_t)Ay << 16);
		if (!(cMaxY > cMinY)) { const int32_t t = cMaxY; cMaxY = cMinY; cMinY = t; }
		if (cMinY > respY) inSeg = false;
		if (cMaxY < respY) inSeg = false;
	}
	const int32_t ddx = (respX - (int32_t)restoreX) >> 10, ddy = (respY - (int32_t)restoreY) >> 10;
	int32_t dist2 = (int32_t)((uint32_t)(ddx * ddx) + (uint32_t)(ddy * ddy));
	if (!inSeg)
	{
		const int32_t adj = dist2 - 0x1d4c;
		if (adj >= 0 && adj > FD(fp, 0x24))
		{
			const int32_t ex = (int32_t)(int16_t)(((int32_t)qX >> 13) - ((int32_t)(uint16_t)Ax << 3));
			const int32_t ey = (int32_t)(int16_t)(((int32_t)qY >> 13) - ((int32_t)(uint16_t)Ay << 3));
			if (ex * ex + ey * ey >= cornerSq) return 0;
			const uint32_t txi = qX >> 15, tyi = qY >> 15;
			const int16_t a2x = (int16_t)((uint16_t)Ax + (uint16_t)Ax), a2y = (int16_t)((uint16_t)Ay + (uint16_t)Ay);
			const uint32_t bearing = ActorBearing(a2x, (int16_t)tyi, a2y, (int16_t)txi);
			uint32_t bi = (bearing + bearing) & 0x3fe;
			int32_t tbl = ActorSin(bi);
			qX = (uint32_t)((int32_t)((uint32_t)(uint16_t)Ax << 16) + tbl * radius);
			const uint32_t bh = ((bi >> 8) + 1) & 3;
			bi = (bi & 0x00ff) | (bh << 8);
			tbl = ActorSin(bi);
			qY = (uint32_t)((int32_t)((uint32_t)(uint16_t)Ay << 16) + tbl * radius);
			hitFlags |= 4;
			portalContinue = 0xffff;
			stats.doorPush++;
			return 0;
		}
	}
	hitFlags |= 4;
	stats.doorPush++;
	qX = (uint32_t)respX;
	qY = (uint32_t)respY;
	if (!((uint32_t)dist2 > 0x100000u)) dist2 = 0x100000;
	SFD(fp, 0x24, dist2);
	portalContinue = 0xffff;
	return 0;
}

void PlayerMove::ClipToDoor(const MoveDoor &d, uint8_t *fp)
{
	// CP:373-398: the four points packed into a wrap-around buffer; edge e is B = point e,
	// A = point e+1. clip always returns CF = 0, so all four edges run.
	stats.doorClip++;
	for (int e = 0; e < 4; e++)
	{
		edgeBx = d.x[e]; edgeBy = d.y[e];
		edgeAx = d.x[(e + 1) & 3]; edgeAy = d.y[(e + 1) & 3];
		ClipCircleToEdge(fp);
	}
}

//==========================================================================
//
// gather_nearby_doors (D:406-451), door_gather_sector (D:392-404), collect_doors_near_query
// (D:347-379): the worklist of what the player can push against.
//
//==========================================================================

void PlayerMove::GatherNearby()
{
	work.clear();
	auto gatherSector = [&](uint16_t key)
	{
		const uint8_t flag = E8(key + 0x16);
		if (flag & 1)
		{
			// find_door_by_sector / find_next_door: every door record keyed by this sector, pool order
			for (size_t i = 0; i < doors.size(); i++)
				if (doors[i].sector == key) work.push_back({ 1, (uint32_t)i });
		}
		if (flag & 2)
		{
			const uint16_t first = E16(4);
			const uint16_t delta = (uint16_t)(key - first);
			const uint16_t index = (uint16_t)((uint16_t)(delta / 0xd) + 2);
			const uint16_t off = RW(ob, index);
			if (off == 0) return;
			uint8_t cnt = off < ob.size() ? ob[off] : 0;
			if (cnt == 0) return;
			uint32_t w = (uint32_t)off + 2;
			const int16_t qx = (int16_t)(qX >> 16), qy = (int16_t)(qY >> 16);
			do
			{
				if (w + 0x10 > ob.size()) break;
				if (!(ob[w + 7] & 0x82) && !(ob[w + 9] & 0x82))
				{
					const int16_t dxv = (int16_t)(RW(ob, w) - (uint16_t)qx);
					const uint16_t adx = dxv < 0 ? (uint16_t)(-dxv) : (uint16_t)dxv;
					const int16_t dyv = (int16_t)(RW(ob, w + 2) - (uint16_t)qy);
					const uint16_t ady = dyv < 0 ? (uint16_t)(-dyv) : (uint16_t)dyv;
					const uint16_t dist = adx > ady ? adx : ady;
					if (dist <= 0x120) work.push_back({ 0, w });
				}
				w += 0x10;
				cnt = (uint8_t)(cnt - 1);
			} while ((int8_t)cnt > 0);
		}
	};
	const uint16_t bx = sector;
	if (bx != 0)
	{
		gatherSector(bx);
		uint8_t cl = E8(bx + 0xd);
		uint16_t nb = E16(bx + 0xe);
		do
		{
			const uint16_t ax = E16(nb + 8);
			if (ax != 0xffff) gatherSector(E16(ax + 6));
			nb = (uint16_t)(nb + 0xc);
			cl = (uint8_t)(cl - 1);
		} while ((int8_t)cl > 0);
	}
	// the creature pool (D:427-447): live slots only; an empty slot is skipped without counting
	for (uint32_t o : pool)
	{
		if (o == 0 || o + 0x10 > ob.size()) continue;
		if (ob[o + 7] & 0x82) continue;
		const int32_t dx = (int16_t)((uint16_t)(qX >> 16) - RW(ob, o));
		if (dx > 0xa0 || dx < -0xa0) continue;
		const int32_t dy = (int16_t)((uint16_t)(qY >> 16) - RW(ob, o + 2));
		if (dy > 0xa0 || dy < -0xa0) continue;
		work.push_back({ 0, o });
	}
	entityCount = (uint32_t)work.size();
}

//==========================================================================
//
// resolve_collisions_against_objects (CP:1213-1400) and rcao_pos_pushout (CP:1180-1197)
//
//==========================================================================

void PlayerMove::PushOut(uint32_t esi, int32_t pushX, int32_t pushY)
{
	const int16_t objX = (int16_t)RW(ob, esi), objY = (int16_t)RW(ob, esi + 2);
	const int16_t qXint = (int16_t)(qX >> 16), qYint = (int16_t)(qY >> 16);
	const uint16_t nxlo = (uint16_t)((int32_t)objX + ((objX < qXint) ? pushX : -pushX));
	const uint16_t nylo = (uint16_t)((int32_t)objY + ((objY < qYint) ? pushY : -pushY));
	const uint32_t newX = (uint32_t)nxlo << 16, newY = (uint32_t)nylo << 16;
	int32_t dX = (int32_t)(qX - newX); if (dX < 0) dX = -dX;
	int32_t dY = (int32_t)(qY - newY); if (dY < 0) dY = -dY;
	stats.objectPush++;
	if ((uint32_t)dY > (uint32_t)dX) qX = newX;
	else qY = newY;
	portalContinue = 0xffff;
}

int PlayerMove::ResolveObjects(uint8_t *fp)
{
	retries = 4;
	for (;;)
	{
		pushed = 0;
		int32_t n = (int32_t)entityCount;
		size_t wi = 0;
		for (;;)
		{
			const WorkItem it = wi < work.size() ? work[wi] : WorkItem{ 0, 0 };
			wi++;
			int16_t zw_dx = 0, zw_ax = 0;
			if (it.flag != 0)
			{
				if (it.ref < doors.size()) ClipToDoor(doors[it.ref], fp);   // CF always 0 (CP:1236-1243)
				goto next_entry;
			}
			{
				const uint32_t esi = it.ref;
				if (esi + 0x10 > ob.size()) goto next_entry;
				const int16_t objX = (int16_t)RW(ob, esi), objY = (int16_t)RW(ob, esi + 2);
				int16_t dxv = (int16_t)((int16_t)(qX >> 16) - objX); if (dxv < 0) dxv = (int16_t)(-dxv);
				int16_t ayv = (int16_t)((int16_t)(qY >> 16) - objY); if (ayv < 0) ayv = (int16_t)(-ayv);
				const uint8_t o7 = ob[esi + 7];
				const uint32_t tbl = (uint32_t)(uint16_t)((uint16_t)RW(ob, esi + 4) << 2);
				auto T8 = [&](uint32_t k) -> uint8_t { return tbl + k < dims.size() ? dims[tbl + k] : 0; };
				auto T16 = [&](uint32_t k) -> uint16_t { return (uint16_t)(T8(k) | (T8(k + 1) << 8)); };
				int16_t narrow_cz;
				if (!(o7 & 1))
				{
					// ISOTROPIC (CP:1253-1324)
					if (!((int16_t)ayv > (int16_t)dxv)) ayv = dxv;
					int32_t rad = (int32_t)T16(2);
					if (rad == 0) goto next_entry;
					rad += boxMax;
					if ((uint16_t)ayv >= (uint16_t)rad) goto next_entry;
					push154 = rad;
					const int16_t das0 = (int16_t)T16(0);
					int16_t whi = FW(fp, 2), wlo = FW(fp, 0);
					const int16_t objZ = (int16_t)RW(ob, esi + 0xa);
					if (o7 & 8)
					{
						if (objZ <= wlo) goto next_entry;
						if ((int16_t)(objZ - das0) >= whi) goto next_entry;
						const int16_t cz = objZ;
						if (cz >= whi) { narrow_cz = (int16_t)(objZ - das0); goto iso_narrow; }
						hitFlags |= 2;
						blocker = esi + 1;
						if ((int16_t)(cz - FW(fp, 0x1e)) > (int16_t)maxStep) { narrow_cz = (int16_t)(objZ - das0); goto iso_narrow; }
						if ((int16_t)(whi - cz) < (int16_t)span) { narrow_cz = (int16_t)(objZ - das0); goto iso_narrow; }
						zw_dx = cz; zw_ax = whi; goto zwrite;
					}
					else
					{
						if (objZ >= whi) goto next_entry;
						if ((int16_t)(objZ + das0) <= wlo) goto next_entry;
						if ((ob[esi + 9] & 4) && fireGate != 0 && host != nullptr)
							host->ContactTrigger(esi);                       // CP:1283-1287
						whi = FW(fp, 2); wlo = FW(fp, 0);
						const int16_t objTop = (int16_t)(objZ + das0);
						if (objTop >= whi) { narrow_cz = objZ; goto iso_narrow; }
						if ((int16_t)(objTop - FW(fp, 0x1e)) > (int16_t)maxStep) { narrow_cz = objZ; goto iso_narrow; }
						if ((int16_t)(whi - objTop) < (int16_t)span) { narrow_cz = objZ; goto iso_narrow; }
						zw_dx = objTop; zw_ax = whi; goto zwrite;
					}
				iso_narrow:
					{
						const int16_t cz = narrow_cz;
						if (cz <= wlo) goto pos_iso;
						const int16_t clamped = (cz < whi) ? cz : whi;
						if ((int16_t)(clamped - wlo) < (int16_t)span) goto pos_iso;
						zw_dx = wlo; zw_ax = clamped; goto zwrite;
					}
				pos_iso:
					hitFlags |= 2;
					pushed = 0xff;
					PushOut(esi, push154, push154);
					goto next_entry;
				}
				else
				{
					// ANISOTROPIC (CP:1326-1389)
					if ((uint32_t)T16(0) == 0 && T16(2) == 0) goto next_entry;
					const unsigned cl = (unsigned)T8(0) & 31;
					const int32_t v15c = (int32_t)((uint32_t)T8(1) << cl);
					int32_t m154 = (int32_t)((uint32_t)T8(2) << cl) + boxMax;
					int32_t m158 = (int32_t)((uint32_t)T8(3) << cl) + boxMax;
					if (((uint8_t)(ob[esi + 6] + 0x20) & 0x40) != 0) { const int32_t t = m154; m154 = m158; m158 = t; }
					push15c = v15c; push154 = m154; push158 = m158;
					if ((uint16_t)ayv >= (uint16_t)m158) goto next_entry;
					if ((uint16_t)dxv >= (uint16_t)m154) goto next_entry;
					int16_t whi = FW(fp, 2), wlo = FW(fp, 0);
					const int16_t objZ = (int16_t)RW(ob, esi + 0xa);
					if (o7 & 8)
					{
						if (objZ <= wlo) goto next_entry;
						if ((int16_t)((int32_t)objZ - v15c) >= whi) goto next_entry;
						const int16_t cz = objZ;
						if (cz >= whi) { narrow_cz = (int16_t)((int32_t)objZ - v15c); goto aniso_narrow; }
						if ((int16_t)(cz - FW(fp, 0x1e)) > (int16_t)maxStep) { narrow_cz = (int16_t)((int32_t)objZ - v15c); goto aniso_narrow; }
						if ((int16_t)(whi - cz) < (int16_t)span) { narrow_cz = (int16_t)((int32_t)objZ - v15c); goto aniso_narrow; }
						zw_dx = cz; zw_ax = whi; goto zwrite;
					}
					else
					{
						if (objZ >= whi) goto next_entry;
						if ((int16_t)((int32_t)objZ + v15c) <= wlo) goto next_entry;
						const int16_t objTop = (int16_t)((int32_t)objZ + v15c);
						if (objTop >= whi) { narrow_cz = objZ; goto aniso_narrow; }
						hitFlags |= 2;
						blocker = esi + 1;
						if ((int16_t)(objTop - FW(fp, 0x1e)) > (int16_t)maxStep) { narrow_cz = objZ; goto aniso_narrow; }
						if ((int16_t)(whi - objTop) < (int16_t)span) { narrow_cz = objZ; goto aniso_narrow; }
						zw_dx = objTop; zw_ax = whi; goto zwrite;
					}
				aniso_narrow:
					{
						const int16_t cz = narrow_cz;
						if (cz <= wlo) goto pos_aniso;
						const int16_t clamped = (cz < whi) ? cz : whi;
						if ((int16_t)(clamped - wlo) < (int16_t)span) goto pos_aniso;
						zw_dx = wlo; zw_ax = clamped; goto zwrite;
					}
				pos_aniso:
					pushed = 0xff;
					PushOut(esi, push154, push158);
					goto next_entry;
				}
			}
		zwrite:
			stats.objectZNarrow++;
			SFW(fp, 0, zw_dx);
			SFW(fp, 2, zw_ax);
		next_entry:
			if (--n <= 0) break;
		}
		if (pushed == 0) return 0;
		retries = (uint8_t)(retries - 1);
		if (retries != 0) continue;
		return 1;
	}
}

} // namespace roth
