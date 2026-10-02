/*
** p_vrdemo.cpp
**
** VR tracking in demos, and the steadied cameras for watching VR play.
** See p_vrdemo.h for why this exists.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <cmath>
#include <cstring>
#include <algorithm>

#include "p_vrdemo.h"
#include "d_protocol.h"

#include "actor.h"
#include "actorinlines.h" // IWYU pragma: keep
#include "c_cvars.h"
#include "d_player.h"
#include "doomstat.h"
#include "g_game.h"
#include "hw_vrmodes.h"
#include "i_time.h"
#include "printf.h"
#include "r_utility.h"
#include "v_text.h"

extern bool demorecording;
extern bool demoplayback;

EXTERN_CVAR(Bool, puristmode)
EXTERN_CVAR(Float, vr_vunits_per_meter)

double P_XYMovement(AActor* mo, DVector2 scroll);

// ---------------------------------------------------------------------------
// Settings for the steadied views. Shared by the live desktop spectator
// (vk_openxrdevice.cpp) and the flat replay camera below.
// ---------------------------------------------------------------------------

// 0 = the desktop window shows the raw eye, as before. 1 = the stabilized spectator view.
CVAR(Int, vr_spectator, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Seconds for the view to cover most of the way to where the head points. Higher = calmer.
CVAR(Float, vr_spectator_smooth, 0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Horizontal field of view of the spectator window, degrees. It is cut out of the eye image,
// so it is capped at what the eye covers; narrower leaves more room to smooth.
CVAR(Float, vr_spectator_fov, 70.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// 1 = the horizon never tilts with the head; 0 = the viewer sees the full head tilt.
CVAR(Float, vr_spectator_rolllock, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// How much of looking up and down the viewer sees, 0..1.
CVAR(Float, vr_spectator_pitchfollow, 0.85f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// A jump bigger than this many degrees in one frame (snap turn, teleport) is a cut, not a pan.
CVAR(Float, vr_spectator_cutangle, 30.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// While a VR demo plays without a headset, follow the recorded head through the stabilizer.
CVAR(Bool, vr_replay_camera, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Print a line for every VR demo frame recorded and replayed (developer aid).
CVAR(Bool, vr_demo_debug, false, 0)

// ---------------------------------------------------------------------------
// FVRViewStabilizer
// ---------------------------------------------------------------------------

static double WrapDeg(double a)
{
	a = std::fmod(a, 360.0);
	if (a > 180.0) a -= 360.0;
	if (a <= -180.0) a += 360.0;
	return a;
}

void FVRViewStabilizer::Update(double targetYaw, double targetPitch, double targetRoll, uint64_t nowNs)
{
	const double follow = std::clamp((double)vr_spectator_pitchfollow, 0.0, 1.0);
	const double rollKeep = 1.0 - std::clamp((double)vr_spectator_rolllock, 0.0, 1.0);
	targetPitch *= follow;
	targetRoll *= rollKeep;

	// A frame gap longer than a quarter second (a menu, a load, a paused window) starts over
	// rather than sweeping across whatever changed meanwhile.
	const double dt = Valid ? (double)(nowNs - LastNs) * 1e-9 : 0.0;
	LastNs = nowNs;
	if (!Valid || dt <= 0.0 || dt > 0.25)
	{
		Valid = true;
		Yaw = targetYaw;
		Pitch = targetPitch;
		Roll = targetRoll;
		return;
	}

	const double dYaw = WrapDeg(targetYaw - Yaw);
	const double dPitch = targetPitch - Pitch;
	const double cut = std::max(1.0, (double)vr_spectator_cutangle);
	if (std::fabs(dYaw) > cut || std::fabs(dPitch) > cut)
	{
		Yaw = targetYaw;
		Pitch = targetPitch;
		Roll = targetRoll;
		return;
	}

	// Exponential approach: the same feel at 72 Hz and at 144 Hz.
	const double tau = std::max(0.0, (double)vr_spectator_smooth);
	const double k = tau > 0.0 ? 1.0 - std::exp(-dt / tau) : 1.0;
	Yaw = WrapDeg(Yaw + dYaw * k);
	Pitch += dPitch * k;
	Roll += (targetRoll - Roll) * k;
}

// ---------------------------------------------------------------------------
// Byte helpers. Big-endian like the rest of the demo stream; doubles as their
// exact bit pattern, because a replayed tic has to see the very same numbers.
// ---------------------------------------------------------------------------

namespace
{

struct FVRFrameWriter
{
	TArray<uint8_t> Bytes;

	void U8(uint8_t v) { Bytes.Push(v); }
	void U16(uint16_t v) { U8(uint8_t(v >> 8)); U8(uint8_t(v)); }
	void I32(int32_t v)
	{
		const uint32_t u = (uint32_t)v;
		U8(uint8_t(u >> 24)); U8(uint8_t(u >> 16)); U8(uint8_t(u >> 8)); U8(uint8_t(u));
	}
	void F64(double v)
	{
		uint64_t u;
		memcpy(&u, &v, sizeof(u));
		for (int shift = 56; shift >= 0; shift -= 8)
			U8(uint8_t(u >> shift));
	}
	void Put(double v) { F64(v); }
	void Put(int v) { I32(v); }
	void Put(bool v) { U8(v ? 1 : 0); }
	void Put(int8_t v) { U8((uint8_t)v); }
	void Put(const DAngle& v) { F64(v.Degrees()); }
	void Put(const DVector2& v) { F64(v.X); F64(v.Y); }
	void Put(const DVector3& v) { F64(v.X); F64(v.Y); F64(v.Z); }
};

struct FVRFrameReader
{
	const uint8_t* Data;
	size_t Size;
	size_t Pos = 0;
	bool Failed = false;

	FVRFrameReader(const uint8_t* data, size_t size) : Data(data), Size(size) {}

	uint8_t U8()
	{
		if (Pos >= Size) { Failed = true; return 0; }
		return Data[Pos++];
	}
	uint16_t U16() { uint16_t hi = U8(); return uint16_t((hi << 8) | U8()); }
	int32_t I32()
	{
		uint32_t u = 0;
		for (int i = 0; i < 4; ++i) u = (u << 8) | U8();
		return (int32_t)u;
	}
	double F64()
	{
		uint64_t u = 0;
		for (int i = 0; i < 8; ++i) u = (u << 8) | U8();
		double v;
		memcpy(&v, &u, sizeof(v));
		return v;
	}
	void Get(double& v) { v = F64(); }
	void Get(int& v) { v = I32(); }
	void Get(bool& v) { v = U8() != 0; }
	void Get(int8_t& v) { v = (int8_t)U8(); }
	void Get(DAngle& v) { v = DAngle::fromDeg(F64()); }
	void Get(DVector2& v) { v.X = F64(); v.Y = F64(); }
	void Get(DVector3& v) { v.X = F64(); v.Y = F64(); v.Z = F64(); }
};

// ---------------------------------------------------------------------------
// WHAT A TIC RECORDS.
//
// Every pawn field the VR backend writes between tics (vk_openxrdevice.cpp
// UpdateControllerState and the grip arbiter, hw_vrmodes.cpp VRMode::SetUp,
// hw_weapon.cpp's laser trace). A field a mod writes itself is harmless here:
// at the start of a tic it already holds what the last tic left, in the
// recording and in a replay that is still in step.
//
// Not recorded: LaserTraceTargetMain/Off, which are object pointers -- a replay
// leaves them to whatever its own renderer finds. The mods treat them as
// cosmetic.
//
// ADD NEW BACKEND-WRITTEN PAWN FIELDS HERE, and bump VRFRAME_VERSION.
// ---------------------------------------------------------------------------

#define VRDEMO_PAWN_FIELDS(X) \
	X(AttackPos) X(AttackPitch) X(AttackAngle) X(AttackRoll) X(MainHandRoll) \
	X(AttackVel) X(AttackAngularVel) \
	X(OffhandPos) X(OffhandPitch) X(OffhandAngle) X(OffhandRoll) \
	X(OffhandVel) X(OffhandAngularVel) \
	X(HmdPos) X(HmdYaw) X(HmdPitch) X(HmdRoll) \
	X(GripContextMain) X(GripContextOff) \
	X(GripHeldMain) X(GripHeldOff) \
	X(GripSubjectMain) X(GripSubjectOff) \
	X(FingerTouchMain) X(FingerTouchOff) \
	X(TriggerValueMain) X(TriggerValueOff) \
	X(GripValueMain) X(GripValueOff) \
	X(ThumbPosMain) X(ThumbPosOff) \
	X(VRTurnYaw) X(TwoHandedHold) \
	X(LaserTraceHitPosMain) X(LaserTraceHitPosOff)

#define VRDEMO_PLAYER_FIELDS(X) \
	X(PlayInVR) X(crouching) X(crouchdir) X(crouchfactor) X(crouchoffset) \
	X(crouchviewdelta) X(viewheight) X(deltaviewheight)

constexpr uint8_t VRFRAME_VERSION = 2;

enum
{
	VRF_HAND0 = 1,	// the main hand's frame is present
	VRF_HAND1 = 2,	// the off hand's frame is present
	VRF_HMD = 4,	// the head's body frame is present
	VRF_AIMOVERRIDE = 8,	// OverrideAttackPosDir as the recording decided it
};

struct FVRMove
{
	uint8_t Kind;
	DVector2 Delta;
};

struct FVRTicFrame
{
	bool Valid = false;
	uint8_t Flags = 0;
	int32_t Tic = 0;

	// The pawn after the between-tic moves: what a replay checks itself against.
	DVector3 CheckPos;
	int32_t CheckHealth = 0;

	TArray<FVRMove> Moves;

	// Indexed by WEAPON HAND (VR_MAINHAND / VR_OFFHAND), with the controller each one was
	// on. Which controller a hand uses is a user setting (vr_control_scheme), so a
	// left-handed viewer must not re-map a right-handed recording -- and the auto-reverse
	// mirror in VRMode::GetWeaponTransform keys off the controller, not the hand.
	double Hand[2][16] = {};
	uint8_t HandController[2] = { 0, 1 };
	double HmdBase[16] = {};
	double HmdPixelStretch = 1.2;
	double HmdBodyYaw = 0;
	double HmdUnitsPerMeter = 34;

	struct
	{
		#define X(f) decltype(AActor::f) f{};
		VRDEMO_PAWN_FIELDS(X)
		#undef X
	} Pawn;

	struct
	{
		#define X(f) decltype(player_t::f) f{};
		VRDEMO_PLAYER_FIELDS(X)
		#undef X
	} Player;
};

// Live side.
TArray<FVRMove> LiveMoves;		// between-tic moves since the last recorded tic
FVRTicFrame Capture;			// this tic, as recorded
TArray<uint8_t> CapturePayload;	// ...and serialised
bool InTic = false;
bool CaptureBroken = false;		// a tic could not be recorded; the rest of this demo has no VR frames

// Replay side.
FVRTicFrame Replay;				// the frame the current tic was given
bool ReplayActive = false;		// this demo has VR frames
bool ReplayWarned = false;		// drift has been reported once
struct FVRReplayHead
{
	bool Valid = false;
	DAngle Yaw, Pitch, Roll;
};
FVRReplayHead ReplayHeadPrev, ReplayHeadCur;
FVRViewStabilizer ReplayStabilizer;

bool IsRecordingPlayer(int player)
{
	return demorecording && !multiplayer && player == consoleplayer
		&& player >= 0 && player < (int)MAXPLAYERS && players[player].mo != nullptr;
}

void CopyMatrix(double dst[16], const VSMatrix& src)
{
	const FLOATTYPE* m = src.get();
	for (int i = 0; i < 16; ++i)
		dst[i] = (double)m[i];
}

void LoadMatrix(VSMatrix* dst, const double src[16])
{
	FLOATTYPE m[16];
	for (int i = 0; i < 16; ++i)
		m[i] = (FLOATTYPE)src[i];
	dst->loadMatrix(m);
}

void Serialise(const FVRTicFrame& f, FVRFrameWriter& w)
{
	w.U8(VRFRAME_VERSION);
	w.U8(f.Flags);
	w.I32(f.Tic);
	w.Put(f.CheckPos);
	w.I32(f.CheckHealth);

	w.U16((uint16_t)f.Moves.Size());
	for (const auto& move : f.Moves)
	{
		w.U8(move.Kind);
		w.Put(move.Delta);
	}

	for (int h = 0; h < 2; ++h)
	{
		if (f.Flags & (h == 0 ? VRF_HAND0 : VRF_HAND1))
		{
			for (int i = 0; i < 16; ++i) w.F64(f.Hand[h][i]);
			w.U8(f.HandController[h]);
		}
	}
	if (f.Flags & VRF_HMD)
	{
		for (int i = 0; i < 16; ++i) w.F64(f.HmdBase[i]);
		w.F64(f.HmdPixelStretch);
		w.F64(f.HmdBodyYaw);
		w.F64(f.HmdUnitsPerMeter);
	}

	#define X(fld) w.Put(f.Pawn.fld);
	VRDEMO_PAWN_FIELDS(X)
	#undef X
	#define X(fld) w.Put(f.Player.fld);
	VRDEMO_PLAYER_FIELDS(X)
	#undef X
}

bool Deserialise(FVRTicFrame& f, FVRFrameReader& r)
{
	const uint8_t version = r.U8();
	if (version != VRFRAME_VERSION)
		return false;

	f.Flags = r.U8();
	f.Tic = r.I32();
	r.Get(f.CheckPos);
	f.CheckHealth = r.I32();

	const unsigned moveCount = r.U16();
	f.Moves.Clear();
	for (unsigned i = 0; i < moveCount && !r.Failed; ++i)
	{
		FVRMove move;
		move.Kind = r.U8();
		r.Get(move.Delta);
		f.Moves.Push(move);
	}

	for (int h = 0; h < 2; ++h)
	{
		if (f.Flags & (h == 0 ? VRF_HAND0 : VRF_HAND1))
		{
			for (int i = 0; i < 16; ++i) f.Hand[h][i] = r.F64();
			f.HandController[h] = r.U8();
		}
	}
	if (f.Flags & VRF_HMD)
	{
		for (int i = 0; i < 16; ++i) f.HmdBase[i] = r.F64();
		f.HmdPixelStretch = r.F64();
		f.HmdBodyYaw = r.F64();
		f.HmdUnitsPerMeter = r.F64();
	}

	#define X(fld) r.Get(f.Pawn.fld);
	VRDEMO_PAWN_FIELDS(X)
	#undef X
	#define X(fld) r.Get(f.Player.fld);
	VRDEMO_PLAYER_FIELDS(X)
	#undef X

	f.Valid = !r.Failed;
	return f.Valid;
}

} // namespace

// ---------------------------------------------------------------------------
// Between-tic moves
// ---------------------------------------------------------------------------

void VR_ApplyRenderMove(player_t* player, EVRRenderMoveKind kind, const DVector2& delta)
{
	if (player == nullptr || player->mo == nullptr)
		return;

	// Noted before it runs, in the order it runs. A replay must not note its own moves.
	if (!demoplayback && IsRecordingPlayer(int(player - players)))
		LiveMoves.Push({ (uint8_t)kind, delta });

	AActor* const mo = player->mo;
	const DVector3 vel = mo->Vel;
	mo->Vel = DVector3(delta, 0);

	// The two kinds settle Z slightly differently. Both are kept exactly as the backend
	// wrote them (vk_openxrdevice.cpp), including the roomscale path's float, because a
	// replay has to land on the same Z to the last bit.
	if (kind == VRMOVE_TELEPORT)
	{
		const bool wasOnGround = mo->Z() <= mo->floorz + 0.1;
		const double oldZ = mo->Z();
		P_XYMovement(mo, DVector2(0, 0));

		if (mo->Z() >= oldZ && wasOnGround)
			mo->SetZ(mo->floorz);
		else
			mo->SetZ(oldZ);
	}
	else
	{
		const bool wasOnGround = mo->Z() <= mo->floorz;
		const float oldZ = (float)mo->Z();
		P_XYMovement(mo, DVector2(0, 0));

		if (mo->Z() >= oldZ && wasOnGround)
			mo->SetZ(mo->floorz);
		else
			mo->SetZ(oldZ);
	}
	mo->Vel = vel;
}

// ---------------------------------------------------------------------------
// The per-tic frame
// ---------------------------------------------------------------------------

void VRDemo_Reset()
{
	LiveMoves.Clear();
	Capture = FVRTicFrame();
	CapturePayload.Clear();
	InTic = false;
	CaptureBroken = false;
	Replay = FVRTicFrame();
	ReplayActive = false;
	ReplayWarned = false;
	ReplayHeadPrev = ReplayHeadCur = FVRReplayHead();
	ReplayStabilizer.Reset();
}

void VRDemo_SetInTic(bool inTic)
{
	InTic = inTic;
}

void VRDemo_BeginTic(int player)
{
	if (!demorecording || demoplayback || player != consoleplayer || CaptureBroken)
		return;

	// Whatever happens below, last tic's capture is no longer this tic's.
	Capture.Valid = false;
	CapturePayload.Clear();

	const VRMode* const vrmode = VRMode::GetVRModeCached(true);
	if (!IsRecordingPlayer(player) || vrmode == nullptr || !vrmode->IsVR())
	{
		// A flat recording carries no frames. Moves cannot happen without a headset,
		// but a stale list must not leak into a later VR tic.
		LiveMoves.Clear();
		return;
	}

	player_t* const pl = &players[player];
	AActor* const mo = pl->mo;
	FVRTicFrame& f = Capture;

	f.Flags = 0;
	f.Tic = gametic;
	f.CheckPos = mo->Pos();
	f.CheckHealth = mo->health;
	f.Moves = std::move(LiveMoves);
	LiveMoves.Clear();

	for (int h = 0; h < 2; ++h)	// VR_MAINHAND, VR_OFFHAND
	{
		const int controller = VR_ControllerForHand(h);
		VSMatrix hand;
		if (vrmode->GetHandTransform(controller, &hand))
		{
			CopyMatrix(f.Hand[h], hand);
			f.HandController[h] = (uint8_t)controller;
			f.Flags |= (h == 0 ? VRF_HAND0 : VRF_HAND1);
		}
	}

	VSMatrix hmdBase;
	double pixelStretch = 1.2;
	float bodyYaw = 0;
	if (vrmode->GetHmdBaseTransform(&hmdBase, &pixelStretch, &bodyYaw))
	{
		CopyMatrix(f.HmdBase, hmdBase);
		f.HmdPixelStretch = pixelStretch;
		f.HmdBodyYaw = bodyYaw;
		f.HmdUnitsPerMeter = VR_UnitsPerMeter();
		f.Flags |= VRF_HMD;
	}

	// UpdateCanonicalMainHandPose's decision for a local VR player (p_user.cpp).
	if (!puristmode)
		f.Flags |= VRF_AIMOVERRIDE;

	#define X(fld) f.Pawn.fld = mo->fld;
	VRDEMO_PAWN_FIELDS(X)
	#undef X
	#define X(fld) f.Player.fld = pl->fld;
	VRDEMO_PLAYER_FIELDS(X)
	#undef X

	FVRFrameWriter w;
	Serialise(f, w);
	if (w.Bytes.Size() > 0xFFFF)
	{
		// Only a frame with thousands of between-tic moves gets here. Recording it
		// truncated would desync silently; drop VR frames for the rest of this demo.
		Printf(TEXTCOLOR_RED "VR demo: a tic was too large to record (%u bytes); this demo will have no VR tracking from here on.\n", w.Bytes.Size());
		f.Valid = false;
		CaptureBroken = true;
		return;
	}
	CapturePayload = std::move(w.Bytes);
	f.Valid = true;

	if (vr_demo_debug)
		Printf("VR demo: tic %d recorded, %u moves, %u bytes\n", f.Tic, f.Moves.Size(), CapturePayload.Size());
}

size_t VRDemo_PendingFrameSize(int player)
{
	if (!IsRecordingPlayer(player) || !Capture.Valid)
		return 0;
	return 1 + 2 + CapturePayload.Size();
}

void VRDemo_WriteFrame(int player, TArrayView<uint8_t>& stream)
{
	if (VRDemo_PendingFrameSize(player) == 0)
		return;

	WriteInt8(DEM_VRFRAME, stream);
	WriteInt16((int16_t)(uint16_t)CapturePayload.Size(), stream);
	WriteBytes(TArrayView<uint8_t>(CapturePayload.Data(), CapturePayload.Size()), stream);
}

void VRDemo_ReadFrame(TArrayView<uint8_t>& stream, int player)
{
	const size_t len = (uint16_t)ReadInt16(stream);
	if (stream.Size() < len)
	{
		AdvanceStream(stream, stream.Size());
		return;
	}
	FVRFrameReader reader(stream.Data(), len);
	FVRTicFrame f;
	const bool ok = Deserialise(f, reader);
	AdvanceStream(stream, len);

	// Only a demo being watched applies these. A recording never reads its own stream.
	if (!demoplayback || !ok || player < 0 || player >= (int)MAXPLAYERS || players[player].mo == nullptr)
		return;

	player_t* const pl = &players[player];
	AActor* const mo = pl->mo;

	if (!ReplayActive)
	{
		ReplayActive = true;
		Printf("VR demo: replaying recorded head and hand tracking.\n");
	}

	// 1) The moves the backend made before this tic, in order.
	for (const auto& move : f.Moves)
		VR_ApplyRenderMove(pl, (EVRRenderMoveKind)move.Kind, move.Delta);

	// 2) Is the replay still where the recording was?
	const double drift = (mo->Pos() - f.CheckPos).Length();
	if (!ReplayWarned && (drift > 0.01 || mo->health != f.CheckHealth))
	{
		ReplayWarned = true;
		Printf(TEXTCOLOR_ORANGE "VR demo: playback no longer matches the recording from tic %d "
			"(%.2f units off, health %d vs %d). Different mods, settings or engine build?\n",
			f.Tic, drift, mo->health, f.CheckHealth);
	}

	// 3) The state the tic is about to read.
	#define X(fld) mo->fld = f.Pawn.fld;
	VRDEMO_PAWN_FIELDS(X)
	#undef X
	#define X(fld) pl->fld = f.Player.fld;
	VRDEMO_PLAYER_FIELDS(X)
	#undef X

	Replay = std::move(f);

	// 4) The head, for the replay camera.
	ReplayHeadPrev = ReplayHeadCur;
	ReplayHeadCur.Valid = true;
	ReplayHeadCur.Yaw = Replay.Pawn.HmdYaw;
	ReplayHeadCur.Pitch = Replay.Pawn.HmdPitch;
	ReplayHeadCur.Roll = Replay.Pawn.HmdRoll;
	if (!ReplayHeadPrev.Valid)
		ReplayHeadPrev = ReplayHeadCur;

	if (vr_demo_debug)
		Printf("VR demo: tic %d replayed, %u moves, drift %.3f\n", Replay.Tic, Replay.Moves.Size(), drift);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool VRDemo_IsReplaying()
{
	return demoplayback && ReplayActive;
}

bool VRDemo_ReplayAimOverride(bool* overrideAim)
{
	if (!VRDemo_IsReplaying() || !Replay.Valid)
		return false;
	*overrideAim = (Replay.Flags & VRF_AIMOVERRIDE) != 0;
	return true;
}

// The frame the PLAYSIM must read, and only while a tic is running. The renderer keeps
// asking the live headset: a replay watched on a monitor then draws its HUD weapon and
// worn models exactly as a flat game does, instead of placing them by a world-space
// matrix the flat HUD projection cannot use.
static const FVRTicFrame* SimFrame()
{
	if (!InTic)
		return nullptr;
	if (VRDemo_IsReplaying())
		return Replay.Valid ? &Replay : nullptr;
	if (demorecording && Capture.Valid)
		return &Capture;
	return nullptr;
}

bool VRDemo_GetWeaponHandTransform(int weaponHand, VSMatrix* out, int* controllerOut)
{
	const FVRTicFrame* const f = SimFrame();
	if (f == nullptr || weaponHand < 0 || weaponHand > 1 || out == nullptr)
		return false;
	if (!(f->Flags & (weaponHand == 0 ? VRF_HAND0 : VRF_HAND1)))
		return false;
	LoadMatrix(out, f->Hand[weaponHand]);
	if (controllerOut)
		*controllerOut = f->HandController[weaponHand] ? 1 : 0;
	return true;
}

bool VRDemo_GetControllerTransform(int controller, VSMatrix* out)
{
	const FVRTicFrame* const f = SimFrame();
	if (f == nullptr || controller < 0 || controller > 1 || out == nullptr)
		return false;
	for (int h = 0; h < 2; ++h)
	{
		if ((f->Flags & (h == 0 ? VRF_HAND0 : VRF_HAND1)) && f->HandController[h] == (uint8_t)controller)
		{
			LoadMatrix(out, f->Hand[h]);
			return true;
		}
	}
	return false;
}

bool VRDemo_GetHmdTransform(VSMatrix* out, const DVector3& bodyOfs, float* outBodyYaw, double yawOverride)
{
	const FVRTicFrame* const f = SimFrame();
	if (f == nullptr || out == nullptr || !(f->Flags & VRF_HMD))
		return false;

	LoadMatrix(out, f->HmdBase);
	const float bodyYaw = std::isnan(yawOverride) ? (float)f->HmdBodyYaw : (float)yawOverride;
	VR_FinishHmdTransform(out, f->HmdPixelStretch, bodyYaw, bodyOfs, f->HmdUnitsPerMeter);
	if (outBodyYaw)
		*outBodyYaw = bodyYaw;
	return true;
}

// ---------------------------------------------------------------------------
// The flat replay camera
// ---------------------------------------------------------------------------

void VRDemo_AdjustReplayView(FRenderViewpoint& viewPoint, const player_t* player)
{
	if (!VRDemo_IsReplaying() || !vr_replay_camera || !ReplayHeadCur.Valid)
		return;
	if (player == nullptr || player != &players[consoleplayer] || player->mo == nullptr || viewPoint.camera != player->mo)
		return;

	// A headset drives its own view; this is for watching on a monitor.
	const VRMode* const vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr && vrmode->IsVR())
		return;

	const double frac = std::clamp(viewPoint.TicFrac, 0.0, 1.0);
	const DAngle yaw = ReplayHeadPrev.Yaw + deltaangle(ReplayHeadPrev.Yaw, ReplayHeadCur.Yaw) * frac;
	const DAngle pitch = ReplayHeadPrev.Pitch + deltaangle(ReplayHeadPrev.Pitch, ReplayHeadCur.Pitch) * frac;
	const DAngle roll = ReplayHeadPrev.Roll + deltaangle(ReplayHeadPrev.Roll, ReplayHeadCur.Roll) * frac;

	ReplayStabilizer.Update(yaw.Degrees(), pitch.Degrees(), roll.Degrees(), I_nsTime());

	viewPoint.Angles.Yaw = DAngle::fromDeg(ReplayStabilizer.Yaw);
	viewPoint.Angles.Pitch = DAngle::fromDeg(std::clamp(ReplayStabilizer.Pitch, -89.0, 89.0));
	viewPoint.Angles.Roll = DAngle::fromDeg(ReplayStabilizer.Roll);
}
