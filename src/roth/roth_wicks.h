#pragma once
//
// roth_wicks.h -- A CANDLE'S WICKS, ONE BY ONE: the pictures, the state, the light. Engine-free (the
// src/roth readers only), so tools/wicks_test runs every part of it against the player's install.
//
// THE OWNER'S DESIGN (docs/WORLD_DESIGN.md 2.3, decided 2026-11-13). The original lights every flame of a
// candle holder with ONE click and ONE art swap (the chain: 0x27 item test, 0x3a re-key, 0x12 delay, 0x0d
// the lit picture, 0x1d the room's light; docs/CHAPTERS.md lists it as timing-sensitive). In VR:
//   - the FIRST wick touched fires the original's use at once, through the VR use path; game state is that
//     one click and nothing else;
//   - each wick SHOWS lit only when the player lights it, in any order: the picture is built from the
//     original's own unlit and lit pictures, per flame (ComposeWicks), at load, from the install;
//   - wicks left unlit catch by themselves 15 s after the first (WickLighting::CATCH_TICS);
//   - once all are lit (or caught) the picture IS the original's lit picture again;
//   - one soft flickering light per holder that grows with each lit wick (CandleLight).
// The per-wick state is presentation: it is never saved, and examine and use see the original's object.
//
// THE PICTURES (BuildWickModel), from tools/wickspots' method (docs/OPENABLES.md 6), here as the code the
// engine runs:
//   1. Both pictures are billboards, drawn bottom-centre on the object: texel column c at (c - W/2) * upp
//      across (W/2 an INTEGER: the engine's sprite offset, roth_texture_object.cpp), row r at (H - r) * upp
//      above the actor's z. The unlit picture is mapped into the lit grid by that rule:
//      dx = W_lit/2 - W_unlit/2, dy = H_lit - H_unlit. Only a pair with the same texel size, the same
//      anchor shift and neither hanging qualifies (the engine's 0x0d swap keeps the actor's z and scale,
//      so only then do the two pictures stand where the original stands them).
//   2. A lit texel is CHANGED when it differs from the unlit one in any lit frame (index 0 is a hole, the
//      object rule; a hole is a value). A texel that is not changed is the same in every frame.
//   3. FLAMES: the texels that differ between the lit picture's own frames, grouped 8-connected; groups
//      whose boxes meet are one flame (a spark). Ordered left to right. Its WICK POINT is the middle of its
//      lowest row, one row down: where the flame meets the wick.
//   4. EVERY CHANGED TEXEL BELONGS TO EXACTLY ONE FLAME (the owner: "assign every changed texel to a flame,
//      by column or nearest flame"):
//        a. inside exactly one flame's column span (x0..x1): that flame -- the flame itself, the candle
//           top under it, its cup rim straight below, the stem texels under the centre flame;
//        b. otherwise the flame whose wick is nearest ACROSS (|x + 0.5 - wick x|) -- a rim or pan that
//           reaches past the candle's own columns goes with the nearest candle;
//        c. a tie across: the flame whose wick is nearer vertically; then the left one.
//      The counts per rule are reported (WickModel::inColumn / byNearest / ties).
//   5. ComposeWicks(mask): a changed texel shows its LIT value when its flame is in the mask, else the
//      unlit value; an unchanged texel shows the unlit value (= every lit frame's). So the full mask is the
//      lit picture, texel for texel and frame for frame, and the empty mask is the unlit picture.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>
#include <string>
#include <vector>

namespace roth
{

struct Image;
struct AnchorOffset;
struct Map;

// A picture as the engine draws an object's art: W across, H down, row-major, 0 = a hole.
struct WickPicture
{
	int W = 0, H = 0;
	double upp = 2.0;            // world units per texel (renderer.c:6512-6525)
	double shift = 0.0;          // the anchor shift (renderer.c:6552-6556)
	bool hang = false;
	std::vector<std::vector<uint8_t>> frames;   // [f][y * W + x]
	bool ok() const { return W > 0 && H > 0 && !frames.empty(); }
	uint8_t At(int f, int x, int y) const { return (x < 0 || y < 0 || x >= W || y >= H) ? 0 : frames[f][(size_t)y * W + x]; }
};

// The stored image (column-major, the quarter turn) as drawn. The texel size and the shift are read exactly
// as TextureSet::Sprite reads them (ReadSpriteModifiers).
WickPicture WickPictureFromImage(const Image &img, const AnchorOffset &anchor);

struct WickFlame
{
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0, texels = 0;   // the flickering texels' box, lit texels
	double cx = 0, cy = 0;       // their centroid (texel centres)
	double wx = 0;               // the wick point: across, the middle of the lowest row
	int wy = 0;                  //                 down, the row under it
	double across = 0, up = 0;   // the wick point in world units: across from the actor (right as drawn), up from its z
	double flameUp = 0;          // the flame's centroid, up from the actor's z
	double flameAcross = 0;
};

struct WickModel
{
	bool ok = false;
	std::string why;             // why not, when !ok
	int W = 0, H = 0, frames = 0;
	double upp = 1.0;
	int dx = 0, dy = 0;          // unlit texel (x, y) is lit texel (x + dx, y + dy)
	std::vector<uint8_t> unlit;                   // the unlit picture in the lit grid (row-major)
	std::vector<std::vector<uint8_t>> lit;        // [f][row-major]
	std::vector<int8_t> owner;                    // per lit texel: -1 unchanged, else its flame
	std::vector<WickFlame> flames;

	// The assignment, counted (rule 4 of the header).
	int changed = 0, flicker = 0, inColumn = 0, byNearest = 0, ties = 0;
	std::vector<int> perFlame;

	int Wicks() const { return (int)flames.size(); }
	uint32_t FullMask() const { return flames.empty() ? 0u : (uint32_t)((1ull << flames.size()) - 1); }
};

// The model of one unlit/lit pair; !ok with `why` when the pair does not qualify (rule 1), when the lit
// picture has no flames or more than kMaxWicks, or when any unlit texel falls outside the lit picture.
constexpr int kMaxWicks = 8;
WickModel BuildWickModel(const WickPicture &unlit, const WickPicture &lit);

// One frame of the picture with the wicks in `mask` lit, row-major W x H (rule 5).
void ComposeWicks(const WickModel &m, uint32_t mask, int frame, std::vector<uint8_t> &out);

//--------------------------------------------------------------------------
// WHICH OBJECTS: every object whose own click chain (base 0x08) swaps ITS OWN picture -- RAW 0x0d with a
// key of 0, the immediate path on the clicked object (raw_commands.c:4533-4548) -- found in the map, never
// typed in. Whether it is a candle is the pictures' question (BuildWickModel: a still picture whose lit
// partner animates only in small flames).
//--------------------------------------------------------------------------
struct LightableObject
{
	int sector = -1, index = -1;
	uint16_t unlitWord = 0;      // the art word it wears in the map
	uint16_t litWord = 0;        // the 0x0d's +0x0c
	uint16_t clickId = 0;        // its command id (the trigger's key)
	uint16_t trigger = 0;        // the 0x08 record, 1-based
	int item = 0;                // the DBASE100 item an 0x27 before the 0x0d tests, 0 for none
};
std::vector<LightableObject> FindLightableObjects(const Map &m);

//--------------------------------------------------------------------------
// THE PER-WICK STATE OF ONE HOLDER. Presentation only: it never touches game state; the one thing it asks
// the game for is the original's use, once, through its caller.
//
//   Flames(near)   once a tic: the wicks a flame is at now. A wick a flame has just come to (not at it last
//                  tic) is an EDGE.
//                  - not started: an edge returns that wick: the caller fires the original's use (the first
//                    wick); whether the use started the original's lighting comes back through Started. A
//                    use that did not (the item test refused) leaves the state as it was, and the next edge
//                    may try again.
//                  - started: every unlit wick a flame is at lights. No use.
//   Started(k)     the original's use started the lighting (its 0x3a re-keyed the holder): wick k is lit.
//   Tick()         a world tic: CATCH_TICS after the first wick every wick is lit (the catch).
//--------------------------------------------------------------------------
class WickLighting
{
public:
	static constexpr int CATCH_TICS = 15 * 35;   // 15 s at the engine's 35 tics (owner, 2026-11-13)

	void Reset(int wicks) { *this = WickLighting(); mWicks = wicks; }
	int Wicks() const { return mWicks; }
	uint32_t Full() const { return mWicks <= 0 ? 0u : (uint32_t)((1ull << mWicks) - 1); }

	// Returns the wick whose edge asks for the use (not started), else -1.
	int Flames(uint32_t near);
	void Started(int k);
	void Tick();

	bool IsStarted() const { return mStarted; }
	uint32_t Mask() const { return mMask; }
	bool AllLit() const { return mStarted && mMask == Full(); }
	bool Caught() const { return mCaught; }
	int TicsSinceStart() const { return mTics; }
	// The tic (counted from Started) each wick was lit at, for the light's ramp; -1 unlit.
	int LitAt(int k) const { return k >= 0 && k < kMaxWicks ? mLitAt[k] : -1; }
	int UsesAsked() const { return mUsesAsked; }

private:
	int mWicks = 0;
	bool mStarted = false, mCaught = false;
	uint32_t mMask = 0, mNear = 0;
	int mTics = 0, mUsesAsked = 0;
	int mLitAt[kMaxWicks] = { -1, -1, -1, -1, -1, -1, -1, -1 };
};

//--------------------------------------------------------------------------
// THE LIGHT OF ONE HOLDER: one soft point that grows with each lit wick (owner, 2026-11-13). Each lit wick
// adds its share, ramped in over kLightRampSec from when it was lit, flickering on its own phase; the point
// sits at the lit flames' weighted centroid; the radius grows as the square root of the lit share (the area
// lit grows with the summed power). Numbers to tune in the headset; modest on purpose.
//--------------------------------------------------------------------------
constexpr double kLightPerWick = 0.30;      // colour scale per wick (effect-light units, 0..64 a channel)
constexpr double kLightRadius = 56.0;       // map units for one wick
constexpr double kLightRampSec = 0.35;
struct CandleLightOut { double across = 0, up = 0, intensity = 0, radius = 0; int lit = 0; };
// `litAgeSec[k]`: seconds since wick k was lit (< 0: unlit). `now`: level seconds (the flicker's clock).
bool CandleLight(const WickModel &m, const double *litAgeSec, double now, CandleLightOut &out);
// One wick's flicker, 0.78 .. 1.0, smooth, deterministic in (now, k).
double WickFlicker(double now, int k);

} // namespace roth
