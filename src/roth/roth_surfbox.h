/*
** roth_surfbox.h
** THE SURFACE BOX: where on a wall face or a floor a click has to land for a RAW trigger record to fire.
**
** ENGINE-FREE (roth_raw.h, roth_surface.h, nothing from GZDoom), so the same code runs in the engine
** (roth_runtime.cpp UseMatch / ExamineMatch, every click channel: the flat use key, the VR finger's far use
** and examine, the palm and fingertip contact) and in tools/surfbox_test, which runs it against the
** original (ROTH.C, the headless build) case by case.
**
** PORT CHECKLIST -- ROTH.C (oracle_build/rothc/roth_c/src/engine) -> here. RC = raw_commands.c,
** RE = renderer.c, IN = input.c. The acceptance test of this port (tools/surfbox_test).
**
** The box (the record)
**  1. words +0x0c x0, +0x0e x1, +0x10 y0, +0x12 y1, signed 16-bit       RC:3034-3035      -> SurfBox
**  2. "no box" is the DWORD at +0x0c being 0 -- both x words 0, not
**     just +0x0c (ROTH_COMMANDS.md's "0 at +0x0c" is a summary)       RC:3031, 3069,
**                                                                        3125, 3175        -> SurfBoxSet
**  3. inside = x0 <= pick+4 <= x1 and y0 <= pick+6 <= y1, inclusive,
**     every compare signed 16-bit                                       RC:3034-3035      -> SurfBoxAdmits
**  4. OUTSIDE THE BOX THE WALK GOES ON to the next record of the list
**     (`continue`); inside, that record is the match and the walk
**     stops, gated or not                                               RC:3036-3044      -> SurfFirstMatch
**  5. the same test on all four boxed channels: use on a face (type 3),
**     use on a floor / platform top (types 2, 8), examine on a face
**     (ch1), examine on a floor / ceiling / platform (ch3)             RC:3027-3045,
**                                                                        3066-3085, 3122-3134,
**                                                                        3172-3184         -> SurfFirstMatch
**  6. the examine-object channel (ch2) has NO box                       RC:3141-3158      -> not tested there
**  7. order of the tests inside the walk: key, not spent (+2 bit 3),
**     the +6 admission, then the box                                    RC:3026-3036      -> SurfFirstMatch
**  8. +6 admission per channel: type 3 use and ch1 dir_mask {1,4,2,1}[+0x1a & 3] (the band); floor use
**     (+6 & 1) ? type 8 : type 2; ch3 {1,2,4,8}[type 2 -> 0, 1 -> 1, 8 -> 2, else 3]
**                                                                        RC:3014, 3027, 3067, 3099-3100,
**                                                                        3121, 3166-3171   -> SurfFlagsAdmit
** The hit texel (the pick record's words +4 and +6, written by the cursor pass of the renderer)
**  9. a WALL: +4 = the texel ALONG the wall, +6 the texel DOWN it      RE:4482-4494 (exit_f_writeout)
** 10. +4: the half-texel accumulator halved, FLIP_X ~a + dimU, then
**     AND the column mask (dimU-1, or 0xffff when the run fits)        RE:4485-4489, 13289, 13338,
**                                                                        13347, 6343     -> WallPickTexel
** 11. +6: the source texel offset (column * row length + row) taken
**     to 16 bits, modulo the row length                                 RE:4492-4494, 4736-4737 -> WallPickTexel
**     NOT REPRODUCED: on a piece whose image has a row count that is not a power of two and that runs past
**     its last row, the original's row mapper keeps a running index that sticks (RE:1929-1935;
**     roth_surface.cpp WrapRow): that row is not a function of the point. WallPickTexel gives the masked
**     row there; tools/surfbox_test compares only the column on such pieces.
** 12. the column / row rule itself is roth_surface.h's WallPieceRule (the visible pass's, which the cursor
**     pass shares: the same column driver, RE:13280-13350)              -> WallPieceRule
** 13. a FLAT (floor, ceiling, platform top or underside): +4 = the high byte of the span's V start <<2 (rp+6),
**     +6 = that of its U start (rp+4)                                   RE:12997-13002, 12950-12963
**     WHAT IS READ AND WHAT IS MEASURED: the writer above is read; how the span's start U / V come from the
**     world point (the per-vertex coordinates rasterize_floorceil_polygon interpolates) is NOT read. The
**     rule is roth_surface.h's FlatTexel (itself confirmed against the original's painted frames,
**     REMAROTH_MEASURED.md 2) with +4 = its ROW and +6 = its COLUMN, each ANDed with the picture's size - 1:
**     measured by tools/surfbox_test against the original's own picks (64x64, 128x64, 32x32 and 256x256
**     opaque pictures; README)                                           -> FlatPickCoords, FlatPickTexel
**
** NOT IN THE ORIGINAL, said here so nobody takes it for the original: a pick for which the port can name no
** texel (SurfTexel::have false) is OUTSIDE every box. The original always has one.
*/

#ifndef ROTH_SURFBOX_H
#define ROTH_SURFBOX_H

#include <stdint.h>
#include <math.h>
#include "roth_raw.h"
#include "roth_surface.h"

namespace roth
{

// The pick record's words +4 and +6.
struct SurfTexel
{
	bool    have = false;
	int16_t x = 0;     // +4
	int16_t y = 0;     // +6
};

// Checklist 1.
struct SurfBox
{
	uint16_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;   // +0x0c, +0x0e, +0x10, +0x12
};

inline SurfBox RecordSurfBox(const Command &c)
{
	SurfBox b;
	b.x0 = c.Word(0x0c); b.x1 = c.Word(0x0e); b.y0 = c.Word(0x10); b.y1 = c.Word(0x12);
	return b;
}

// Checklist 2.
inline bool SurfBoxSet(const SurfBox &b) { return b.x0 != 0 || b.x1 != 0; }

// Checklist 3.
inline bool SurfBoxAdmits(const SurfBox &b, const SurfTexel &t)
{
	if (!SurfBoxSet(b)) return true;
	if (!t.have) return false;
	return (int16_t)b.x0 <= t.x && t.x <= (int16_t)b.x1 && (int16_t)b.y0 <= t.y && t.y <= (int16_t)b.y1;
}

enum class SurfChannel : uint8_t
{
	UseFace,       // dispatch_entry_command_trigger, type 3
	UseFlat,       // dispatch_entry_command_trigger, types 2 / 8
	ExamineFace,   // dispatch_entry_command_trigger_b, channel 1
	ExamineFlat,   // dispatch_entry_command_trigger_b, channel 3
};

// The band bit of a wall pick: dir_mask{1,4,2,1}[+0x1a & 3] (RC:3014, 3099). 0x01 mid, 0x04 upper, 0x02 lower.
inline uint8_t SurfBandBit(uint8_t bandIndex)
{
	static const uint8_t dm[4] = { 0x01, 0x04, 0x02, 0x01 };
	return dm[bandIndex & 3];
}

// Checklist 8. `bandBit` is SurfBandBit's (the faces); 0 means "the band is not known" and skips the band --
// a port case (a pick with no tier), never reached from the original.
inline bool SurfFlagsAdmit(SurfChannel ch, uint8_t f6, uint8_t pickType, uint8_t bandBit)
{
	switch (ch)
	{
	case SurfChannel::UseFace:
	case SurfChannel::ExamineFace:
		return bandBit == 0 || (f6 & bandBit) != 0;
	case SurfChannel::UseFlat:
		return (f6 & 1) ? pickType == 8 : pickType == 2;
	case SurfChannel::ExamineFlat:
	{
		static const uint8_t dm2[4] = { 1, 2, 4, 8 };
		const int idx = pickType == 2 ? 0 : pickType == 1 ? 1 : pickType == 8 ? 2 : 3;
		return (f6 & dm2[idx]) != 0;
	}
	}
	return false;
}

// One record of a channel's list for one target (a face or a sector), as the walk reads it.
struct SurfCandidate
{
	uint16_t record = 0;     // 1-based command index
	uint8_t  modifier = 0;   // +2, live
	uint8_t  fireFlags = 0;  // +6
	SurfBox  box;
};

// Checklist 4, 5, 7: the index in `list` of the record the original's walk stops at, or -1. The caller then
// runs the flow pre-pass on THAT record only (a gated record is no match, and no later record is tried:
// RC:3040-3044, 3080-3082).
template <class List>
int SurfFirstMatch(const List &list, SurfChannel ch, uint8_t pickType, uint8_t bandBit, const SurfTexel &t)
{
	int i = 0;
	for (const SurfCandidate &c : list)
	{
		const int at = i++;
		if (c.modifier & 0x08) continue;
		if (!SurfFlagsAdmit(ch, c.fireFlags, pickType, bandBit)) continue;
		if (!SurfBoxAdmits(c.box, t)) continue;
		return at;
	}
	return -1;
}

// Checklist 9-12. The pick record's +4 / +6 for a point `d` along a wall face from its first vertex, at height
// `z`, on the piece `p` (WallPieceRule) of an image dimU along the wall by dimV down it. have = false where the
// piece draws nothing at z (the cursor pass then has no pixel of this piece).
inline SurfTexel WallPickTexel(const WallPiece &p, int dimU, int dimV, double d, double z)
{
	SurfTexel t;
	if (!p.drawn || dimU <= 0 || dimV <= 0 || z > p.top || z < p.bot) return t;
	int c = (int)floor(p.u0 + p.su * d + 1e-7);
	uint16_t a = (uint16_t)c;
	if (p.mirror) a = (uint16_t)((uint16_t)~a + (uint16_t)dimU);                     // RE:4488
	a &= p.uMasked ? (uint16_t)(dimU - 1) : (uint16_t)0xffff;                       // RE:4489
	int r = (int)floor(p.v0 + p.sv * (p.anchor - z) + 1e-7);
	if (p.vMasked) r = (int)((uint16_t)r & (uint16_t)(dimV - 1));
	const uint16_t off = (uint16_t)((uint32_t)a * (uint32_t)dimV + (uint32_t)(uint16_t)r);   // RE:4492
	t.have = true;
	t.x = (int16_t)a;
	t.y = (int16_t)(uint16_t)(off % (uint16_t)dimV);                                // RE:4494
	return t;
}

// Checklist 13: the continuous texel coordinates of a world point on a flat (roth_surface.h FlatTexel's, before
// its wrap): col along the stored row, row down the stored rows.
inline void FlatPickCoords(const FlatSetup &s, double worldX, double worldY, double &col, double &row)
{
	const double upt = FlatUnitsPerTexel(s);
	const double k = s.opaque256 ? 1.0 : 0.5;
	col = -worldX / upt + k * s.shiftX;
	row = +worldY / upt - k * s.shiftY;
	if (s.mirrorX) col = -col;
	if (s.mirrorY) row = -row;
}

inline SurfTexel FlatPickTexel(const FlatSetup &s, double worldX, double worldY)
{
	SurfTexel t;
	double col, row;
	FlatPickCoords(s, worldX, worldY, col, row);
	if (s.texW == 0 || s.texH == 0) return t;
	t.have = true;
	t.x = (int16_t)((int)floor(row + 1e-7) & (s.texH - 1));   // +4: the row (down the stored rows), AND the row mask
	t.y = (int16_t)((int)floor(col + 1e-7) & (s.texW - 1));   // +6: the column (along a stored row), AND the column mask
	return t;
}

} // namespace roth

#endif // ROTH_SURFBOX_H
