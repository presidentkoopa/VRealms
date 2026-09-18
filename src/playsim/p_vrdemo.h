/*
** p_vrdemo.h
**
** VR tracking in demos, and the steadied cameras for watching VR play.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** WHY THIS EXISTS
**
** A demo replays usercmds, and a single-player VR game is not driven by
** usercmds alone. Between tics the VR backend writes the pawn directly:
** the hand and head fields (AttackPos, OffhandPos, Hmd*, grip and trigger
** values...), the crouch factor from head height, and -- through
** P_XYMovement, once per rendered frame -- roomscale steps and teleports.
** The playsim then reads all of that, and VRMode::GetWeaponTransform, live.
** None of it was in the demo, so a VR demo drifted from its first tic.
**
** So each tic of a VR recording carries one DEM_VRFRAME: every
** between-tic pawn move since the last tic, in order, and the VR state the
** tic is about to read. Playback replays the moves through the same
** function the backend uses (VR_ApplyRenderMove) and writes the state
** back, at the same point in the tic, before the tic's other commands.
**
** WHAT IS RECORDED is listed once, in FVRTicFrame (p_vrdemo.cpp). A new
** field the VR backend writes into the pawn between tics has to be added
** there, or demos stop matching when a mod starts reading it. Playback
** checks the pawn's position against the recording every tic and says so
** the first time they disagree.
**
** Only the Vulkan OpenXR backend reports its moves; it is the one this fork
** runs. Multiplayer is untouched: it already rebuilds aim from the usercmd.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include <cstdint>
#include <cstddef>
#include "tarray.h"
#include "vectors.h"
#include "matrix.h"

class player_t;
struct FRenderViewpoint;

// ---------------------------------------------------------------------------
// Between-tic pawn moves (vk_openxrdevice.cpp)
// ---------------------------------------------------------------------------

enum EVRRenderMoveKind : uint8_t
{
	VRMOVE_ROOMSCALE = 0,	// the head moved in the room this frame
	VRMOVE_TELEPORT = 1,	// a single-player teleport landed this frame
};

// One between-tic move, exactly as the VR backend has always applied it:
// the pawn's velocity is swapped for 'delta', P_XYMovement runs, the old
// velocity comes back, and the Z is settled per kind. Live play and demo
// playback both go through here, and a recording notes every call.
void VR_ApplyRenderMove(player_t* player, EVRRenderMoveKind kind, const DVector2& delta);

// ---------------------------------------------------------------------------
// The per-tic frame (g_game.cpp, d_net.cpp)
// ---------------------------------------------------------------------------

void VRDemo_Reset();                                               // recording/playback start and stop
void VRDemo_BeginTic(int player);                                  // G_Ticker, before RunPlayerCommands
void VRDemo_SetInTic(bool inTic);                                  // G_Ticker, around the tic
size_t VRDemo_PendingFrameSize(int player);                        // bytes VRDemo_WriteFrame will write; 0 = none
void VRDemo_WriteFrame(int player, TArrayView<uint8_t>& stream);   // G_WriteDemoTiccmd, before the specials
void VRDemo_ReadFrame(TArrayView<uint8_t>& stream, int player);    // Net_DoCommand(DEM_VRFRAME)

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

// A demo that carries VR frames is playing back. The headset, if one is on,
// is not the source of the pawn's VR state while this is true.
bool VRDemo_IsReplaying();

// The OverrideAttackPosDir decision the recording made (it depends on
// user settings that may differ at playback). False = no recorded value.
bool VRDemo_ReplayAimOverride(bool* overrideAim);

// A hand frame the playsim must read instead of the live one: the recorded
// frame during playback, and during a recorded tic the frame captured at its
// start -- the same value the live call returns, but guaranteed to be the one
// written to the demo. Both answer only while a tic is running; the renderer
// keeps reading the live headset.
//
// By WEAPON HAND, reporting which controller that hand was on when it was
// recorded, because the auto-reverse mirror keys off the controller and the
// hand-to-controller map is a user setting.
bool VRDemo_GetWeaponHandTransform(int weaponHand, VSMatrix* out, int* controllerOut);
// By controller, for callers that ask that way.
bool VRDemo_GetControllerTransform(int controller, VSMatrix* out);

// The same for VRMode::GetHmdTransform. Finishes the recorded base with the
// caller's seat offset and yaw override.
bool VRDemo_GetHmdTransform(VSMatrix* out, const DVector3& bodyOfs, float* outBodyYaw, double yawOverride);

// ---------------------------------------------------------------------------
// Steadied cameras
// ---------------------------------------------------------------------------

// Smooths a view for someone who is not wearing the headset: yaw and pitch
// ease toward the target, pitch follows only part of the way, roll is
// levelled, and a jump bigger than vr_spectator_cutangle (a snap turn, a
// teleport) is taken as a cut. Angles in degrees, any convention, as long
// as the caller is consistent.
struct FVRViewStabilizer
{
	bool Valid = false;
	double Yaw = 0, Pitch = 0, Roll = 0;
	uint64_t LastNs = 0;

	void Reset() { Valid = false; }
	void Update(double targetYaw, double targetPitch, double targetRoll, uint64_t nowNs);
	void Force(double yaw, double pitch, double roll) { Yaw = yaw; Pitch = pitch; Roll = roll; }
};

// The flat replay camera (r_utility.cpp, R_InterpolateView): while a VR demo
// plays without a headset, the view follows the recorded head through the
// stabilizer instead of the pawn's body angles. The chase camera ("chase")
// orbits the steadied view.
void VRDemo_AdjustReplayView(FRenderViewpoint& viewPoint, const player_t* player);
