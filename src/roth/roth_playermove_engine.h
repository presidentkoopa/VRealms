#pragma once
//
// The original's player movement (roth_playermove.h) driving the engine's Realms player.
// Called from roth_runtime.cpp (the level, the tic), p_user.cpp (the command), g_game.cpp
// (keyboard turning) and p_vrdemo.cpp (the headset's roomscale steps and teleports).
//
// What it replaces: for the Realms player the engine's own thrust, friction, gravity, step-up,
// jump, crouch and line collision do nothing (the command's moves are taken off it before the
// pawn thinks); the position, angle, eye height and sector come from the port every tic.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>
#include <vector>

struct FLevelLocals;
struct player_t;

namespace roth
{

struct Map;
class Log;

// The RAW file's bytes, kept by the loader before it parses them.
void PlayerMoveSetRaw(const std::vector<uint8_t> &bytes);
// After the actors (their tables give the collision dimensions). `map` is the live map.
void PlayerMoveBeginLevel(const Map &map, FLevelLocals *level, Log *log);
void PlayerMoveEndLevel();
// Does the port move this player? (a Realms level is up and this is its player)
bool PlayerMoveOwns(const player_t *player);
// Flat keyboard turning is the original's turn ramp, so the engine must not turn as well.
bool PlayerMoveOwnsTurnKeys();
// P_PlayerThink, before the pawn thinks: read the held keys, the sticks, jump and crouch from the
// command, then take the moves off it so the engine does not move the pawn too.
void PlayerMoveTakeCmd(player_t *player);
// Once a tic (TickLevelLogic): two 70 Hz ticks, one frame of the original, the pawn written.
void PlayerMoveTick(FLevelLocals *level);
// VR_ApplyRenderMove: a roomscale step (through the original's collision) or a teleport.
// Returns false when the port does not own the player (the engine's own path runs).
bool PlayerMoveRenderMove(player_t *player, bool teleport, double dx, double dy);

} // namespace roth
