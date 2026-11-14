#pragma once
//
// roth_lightramp.h -- THE ORIGINAL'S LIGHT RAMP (opcode 0x1d) AND ITS BYTE ADD, engine-free, so the runtime
// (roth_runtime.cpp ChangeLighting / TickChangeLighting / ApplyLightDelta) and a test against the running original
// (tools/held_test `ramp`) run the same lines.
//
// FOUND 2026-11-14 (the niche's third light change rose to 255 on the owner's PC, held_pc_run_367c710c): the port
// read the step byte at +7 as UNSIGNED, so a step with bit 7 set -- a DOWN ramp, STUDY1 #433's 0xe2 = -30 -- became
// an up ramp of 226, and the byte add clamped at 255 where the original's wraps. 23 0x1d records in the 44 retail
// maps carry such a step (RAQUIA5, MAS3, AELF, SALVAT x16, STUDY3, STUDY4, STUDY1 #433 and #591).
//
// Checklist (ROTH.C, oracle_build/rothc/roth_c/src/engine/raw_commands.c -> here):
//  1. cmd_change_lighting :4390-4412: cl = byte[rec+7]                                     -> LightRampPayload
//  2.   if byte[rec+2] & 2: cl = -cl (8 bits)                                              -> LightRampPayload
//  3.   word[eff+6] = (cl & 0x80) ? 0xff00 | (uint8)(-cl) : cl                             -> LightRampPayload
//     (registration, the 0x21 guard, rec[6]&1 flipping rec[2]^2 and the base stamp stay in ChangeLighting)
//  4. tick_change_lighting :1788-1806: al = low byte (remaining), ah = high byte (down)    -> LightRampTick
//  5.   al == 0: finished (the record's finalize stays in TickChangeLighting)              -> LightRampTick false
//  6.   dl = min(frame scale, 0x40); if al <= dl: dl = al; al -= dl                        -> LightRampTick
//  7.   ah != 0: dl = -dl (8 bits)                                                         -> LightRampTick
//  8. apply_light_delta_to_record_list :5668-5684: a nonzero byte takes byte + dl, 8-bit  -> LightByteAdd
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <cstdint>

namespace roth
{

// The ramp word cmd_change_lighting stores (items 1-3): low byte the amount still to go, high byte 0xff = down.
inline uint16_t LightRampPayload(uint8_t stepByte, uint8_t modifier)
{
	uint8_t cl = stepByte;
	if (modifier & 0x02) cl = (uint8_t)(0u - cl);
	return (cl & 0x80) ? (uint16_t)(0xff00u | (uint8_t)(0u - cl)) : (uint16_t)cl;
}

// One step of tick_change_lighting (items 4-7). False when the ramp had nothing left (it is finished); otherwise
// `delta` is the byte to add to every sector of the group, and the payload's remaining amount is updated.
inline bool LightRampTick(uint16_t &payload, uint32_t frameScale, uint8_t &delta)
{
	uint8_t al = (uint8_t)(payload & 0xff);
	const uint8_t ah = (uint8_t)(payload >> 8);
	if (al == 0) { delta = 0; return false; }
	uint32_t dl = frameScale >= 0x40 ? 0x40 : frameScale;
	if (al <= (uint8_t)dl) dl = al;
	al = (uint8_t)(al - (uint8_t)dl);
	payload = (uint16_t)((payload & 0xff00u) | al);
	delta = ah != 0 ? (uint8_t)(0u - (uint8_t)dl) : (uint8_t)dl;
	return true;
}

// apply_light_delta_to_record_list's add (item 8): a sector authored at 0 is immune; otherwise an 8-bit add.
inline uint8_t LightByteAdd(uint8_t light, uint8_t delta) { return light != 0 ? (uint8_t)(light + delta) : light; }

} // namespace roth
