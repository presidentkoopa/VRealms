/*
** p_vrreplay.h
**
** [WEAPONWORLDAUDIT] A FILE-DRIVEN HAND INPUT SOURCE.
**
** The weapon spec (WEAPONWORLDAUDIT/VR_Weapon_Spec.md §8.2) wants a reload to be provable
** without a headset: a recorded or hand-authored stream of controller poses and buttons is
** fed into the tic command, the sim runs exactly as it would for a real player, and a
** telemetry log (p_vrtelemetry) shows what the weapon did. Same file, same log.
**
** This is NOT the binary VR demo path (p_vrdemo.cpp), which rides inside -record demos and
** cannot be authored by hand. This one is a plain text file a person or a script can write:
**
**   # vrreplay 1
**   # rep  mh.fwd mh.left mh.up mh.yaw mh.pitch mh.roll  oh.fwd ... oh.roll  head.z head.pitch head.roll
**   #      grip0 grip1 trig0 trig1 bits0 bits1 thumb0 thumb1 thumb2 thumb3  buttons
**   35   0 0 0 0 0 0   0 0 0 0 0 0   0 0 0   0 0 0 0 0 0 0 0 0 0   0
**
** Every value is the RAW WIRE INTEGER the usercmd carries (positions map units x16, angles
** BAM16, analogs 0-255, bits per EVRHandBits, buttons the BT_* mask). No conversion happens
** here on purpose: the file round-trips byte-exactly with what the engine would have sent,
** and the human units live in the authoring script (WEAPONWORLDAUDIT/tools/vrreplay.py).
** 'rep' repeats the row for that many tics. '#' starts a comment. Blank lines are skipped.
**
** The hook sits in G_BuildTiccmd AFTER the VR mode has had its say, and runs whether or not a
** VR mode is active, so a headless build can drive the sim's hands. A playing file overrides
** the VR fields of the command (and ORs its buttons); it never touches movement or aim.
**
** cvars (none archived; a replay is a session thing):
**   vr_replay_play   <file>   read rows from this file, one per tic, from the next tic on
**   vr_replay_record <file>   append the command's VR fields to this file, one row per tic
**   vr_replay_loop   bool     when the file ends, start again (default: stop and clear)
*/
#pragma once

struct usercmd_t;

// Called from G_BuildTiccmd once per built command, after the VR mode wrote its part.
void VRReplay_OnBuildTiccmd(usercmd_t* cmd);

// True while a replay file is being played into the command.
bool VRReplay_Playing();

// The 1-based row number most recently applied, 0 when not playing. For the telemetry log.
int VRReplay_Row();
bool VRReplay_IsPlaying();	// [NETPLAY] a replay file is driving the hands this tic
