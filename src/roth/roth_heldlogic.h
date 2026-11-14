#pragma once
//
// roth_heldlogic.h -- THE HELD OBJECTS' RULES, ENGINE-FREE: the match struck on the Matchbox's side, its 20 s life and
// its flame point; the Lamp on its hip hook, grabbed and released; the niche's light following the original's light
// byte; the forced turn's view snap; the Book opened in both hands and the key that drops into the palm; which picture
// each hand shows. Plain C++ (no engine headers), tested by tools/held_test. The engine half is roth_held.cpp.
//
// THE OWNER'S DECISIONS (docs/VR_CHAPTER01_SHADOWS.md 6, 2026-11-13 evening), and what each piece below is:
//   Q1  strike a real match: the Matchbox stays the right-hand item (the game's item test passes); strike it on the
//       box's side; a lit match in the OFF hand for 20 s; its flame lights the wicks       -> StrikeDetector, Match
//   Q2  the Lamp on a hip hook, grabbed when needed; it lights the area around the player
//       from the hip; grab it to hold it up                                                 -> LampHook
//   Q7  the forced turn (0x3f) after card 2 snaps during the black frames                   -> ForcedTurnView
//   Q8  the Book opens in both hands, its film beside it; the silver key drops into the
//       palm at the original's first-look give, then goes to the inventory as the original -> BookView
//   Q10 the niche flash: a real dynamic light pulsing with the original's light pulses      -> LightFollower
//   Q12 a picked-up item shows in the item hand when the original selects it               -> HandShows
//
// NONE OF THIS IS GAME STATE. Every input is read (hand points, buttons, the original's inventory, its light bytes,
// its popup); every output is presentation (where a picture or a light is drawn, which pose a hand takes) or a
// request the engine turns into the ORIGINAL's own click -- the match at a wick asks for the use the matchbox hand
// would make (roth_candles.cpp), nothing else. The differential test (tools/held_test/run_diff.sh) plays what these
// rules ask for into the running original and compares its game state with the flat clicks'.
//
// UNITS: metres and seconds unless a name says tics (the engine's 35 Hz tic, the rate everything here is sampled at).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <cmath>
#include <cstdint>

namespace roth
{
namespace held
{

struct V3
{
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	V3 operator+(const V3 &o) const { return V3(x + o.x, y + o.y, z + o.z); }
	V3 operator-(const V3 &o) const { return V3(x - o.x, y - o.y, z - o.z); }
	V3 operator*(double s) const { return V3(x * s, y * s, z * s); }
	double Dot(const V3 &o) const { return x * o.x + y * o.y + z * o.z; }
	V3 Cross(const V3 &o) const { return V3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x); }
	double Length() const { return std::sqrt(x * x + y * y + z * z); }
	V3 Unit() const { const double l = Length(); return l > 1e-12 ? *this * (1.0 / l) : V3(); }
};

constexpr double kTicSeconds = 1.0 / 35.0;   // the engine's tic
constexpr int kTicsPerSecond = 35;

//==========================================================================
// THE MATCHBOX AND THE STROKE (docs/ITEM_DESIGN.md 16, "Object" and "VR" step 4)
//==========================================================================

// The box: 5.5 x 3.6 x 1.6 cm, brown striking strips on the two long narrow sides (ITEM_DESIGN 16 "Object").
constexpr double kBoxLength = 0.055, kBoxWidth = 0.036, kBoxThick = 0.016;
// The stroke: the head within 1 cm of a strip's face, moving along it faster than about 0.4 m/s relative to the box,
// over at least 3 cm (ITEM_DESIGN 16 "VR" step 4). It lights on that stroke.
constexpr double kStrikeNear = 0.010;      // outside the strip's face
constexpr double kStrikeInside = 0.004;    // and this far into the box (a virtual box: the fingertip may sink in)
constexpr double kStrikeSpeed = 0.40;      // m/s along the long axis, relative to the box
constexpr double kStrikeRun = 0.030;       // m along the long axis, in contact, one direction

// Where the box is this tic: its centre and three unit axes (the long axis, the width axis the strips face along,
// the thickness), in one world frame, metres.
struct BoxFrame
{
	V3 centre, along, across, thick;
	V3 Local(const V3 &p) const { const V3 d = p - centre; return V3(d.Dot(along), d.Dot(across), d.Dot(thick)); }
};

// A stroke along a striker: sampled once a tic with the striking point (the off hand's index fingertip) and the box.
// The segment between two samples is clipped to each strip's contact slab IN THE BOX'S OWN FRAME, so a fast stroke
// that crosses the whole strip between two tics still counts, and the box moving with the hand does not.
class StrikeDetector
{
public:
	void Reset() { mHave = false; mRun = 0; mDir = 0; mStrip = 0; mLastSpeed = 0; mInContact = false; }
	// True on the sample that completes a stroke (then it starts over).
	bool Sample(const BoxFrame &box, const V3 &head, double dtSeconds);

	// For the log: the stroke so far.
	double Run() const { return mRun; }
	double LastSpeed() const { return mLastSpeed; }
	int Strip() const { return mStrip; }           // +1 / -1: which strip the run is on, 0 none
	bool InContact() const { return mInContact; }

	// The contact slab of strip `side` (+1 / -1) in the box's frame: [lo, hi] on each axis.
	static void Slab(int side, V3 &lo, V3 &hi);
	// The part of segment a -> b inside the slab, as parameters [t0, t1] of the segment; false when none.
	static bool Clip(const V3 &a, const V3 &b, const V3 &lo, const V3 &hi, double &t0, double &t1);

private:
	bool mHave = false;
	V3 mLast;               // the last sample, in the box's frame
	double mRun = 0;        // along the long axis, in contact, this direction
	int mDir = 0;           // +1 / -1 along the long axis
	int mStrip = 0;
	double mLastSpeed = 0;
	bool mInContact = false;
};

//==========================================================================
// THE MATCH (owner Q1: "a lit match in the off hand for 20 s"; ITEM_DESIGN 16 "VR" step 5)
//==========================================================================

constexpr int kMatchTics = 20 * kTicsPerSecond;    // 20 s
constexpr int kMatchWarnTics = 2 * kTicsPerSecond; // the last 2 s: the flame reaches the fingers (a haptic warning)
// The stick held in the pinch (fbx_pinch, Blender 694): laid along the index finger, its head this far beyond the
// fingertip, its end this far behind it; the flame stands just beyond the head. A match is about 4.5 cm long.
constexpr double kStickAhead = 0.032, kStickBehind = 0.013, kFlameBeyond = 0.006;

class Match
{
public:
	void Reset() { mLit = false; mAge = 0; mStruck = 0; }
	// A stroke on the striker: lights a fresh match when none burns. True when it lit one.
	bool Strike() { if (mLit) return false; mLit = true; mAge = 0; mStruck++; return true; }
	// One world tic (the burn stands still while the world does: the inventory pauses it, as the candles' catch).
	// True on the tic it goes out.
	bool Tick() { if (!mLit) return false; if (++mAge >= kMatchTics) { mLit = false; return true; } return false; }
	void PutOut() { mLit = false; }
	bool Lit() const { return mLit; }
	int Age() const { return mAge; }
	int TicsLeft() const { return mLit ? kMatchTics - mAge : 0; }
	bool Warning() const { return mLit && TicsLeft() <= kMatchWarnTics; }
	int Struck() const { return mStruck; }

	// The flame's own flicker, 0.78..1.0, a function of time alone (the candles' range, roth_wicks.h CandleLight).
	static double Flicker(double seconds);
	// The stick and the flame from the pinching hand: the index fingertip and the finger's direction (knuckle to tip).
	static V3 FlamePoint(const V3 &indexTip, const V3 &fingerDir) { return indexTip + fingerDir.Unit() * (kStickAhead + kFlameBeyond); }
	static V3 StickHead(const V3 &indexTip, const V3 &fingerDir) { return indexTip + fingerDir.Unit() * kStickAhead; }
	static V3 StickEnd(const V3 &indexTip, const V3 &fingerDir) { return indexTip - fingerDir.Unit() * kStickBehind; }

private:
	bool mLit = false;
	int mAge = 0;
	int mStruck = 0;
};

//==========================================================================
// THE LAMP ON ITS HOOK (owner Q2; ITEM_DESIGN 15 "VR": the bail, curl 206; the hook at the hip on the off-hand side)
//==========================================================================

// Where the hook is, from the player's body: on the off-hand side, at the hip, a little forward (a coat hook on the
// belt). Fractions of the eye height above the feet and metres sideways / forward. The bail hangs from it.
constexpr double kHookHeight = 0.56;       // x the eye height: the hip
constexpr double kHookSide = 0.20, kHookForward = 0.06;
// The globe hangs this far below the bail (the bail's loop to the flame): ITEM_DESIGN 15 "Object", 30 cm tall with the
// bail above, the flame a third of the way down.
constexpr double kGlobeBelowBail = 0.14;
// A hand whose palm is this close to the bail and closes its grip takes the lamp.
constexpr double kLampGrabReach = 0.12;

class LampHook
{
public:
	enum { ON_HOOK = -1 };
	void Reset() { mHand = ON_HOOK; mGripWas[0] = mGripWas[1] = false; mPrimed[0] = mPrimed[1] = false; }
	// Whether a grip closing now at `palm` would take the lamp (the picker leaves that press alone): the lamp on the
	// hook, or already in this hand.
	bool WantsGrip(int role, const V3 &palm, const V3 &bail) const;
	// One tic. For each role (0 main, 1 off): whether the hand is tracked, its palm, whether its grip is closed.
	// `bail`: where the bail is now (on the hook, or in the holding hand). Returns +1 on a grab, -1 on a release, 0.
	int Tick(const bool valid[2], const V3 palm[2], const bool grip[2], const V3 &bail);
	int Hand() const { return mHand; }          // the role holding it, or ON_HOOK
	bool OnHook() const { return mHand == ON_HOOK; }
	// Where the hook is for a body standing at `feet` facing `forward` (level unit vector) with `right` its right,
	// the eye `eyeHeight` above the feet, the off hand on the `offRight` side (+1 right, -1 left). Metres.
	static V3 HookPoint(const V3 &feet, const V3 &forward, const V3 &right, const V3 &up, double eyeHeight, int offRight);
	static V3 GlobeBelow(const V3 &bail, const V3 &up) { return bail - up * kGlobeBelowBail; }

private:
	int mHand = ON_HOOK;
	bool mGripWas[2] = { false, false };
	bool mPrimed[2] = { false, false };     // the grip was read last tic: a grip already closed is not a grab
};

//==========================================================================
// A LIGHT THAT FOLLOWS THE ORIGINAL'S LIGHT BYTE (owner Q10: the niche flash)
//==========================================================================

// The niche (STUDY1 light group 46, sector s58): the tail of the tall candles' chain ramps it +40, waits 310, +30,
// waits 70, -30 (records #430, #431, #433; docs/VR_CHAPTER01_SHADOWS.md entry 34). The light follows the byte: no
// timer of its own. Full intensity at the highest the chain takes it above the pack's value (+40 +30).
constexpr int kFollowFull = 70;

class LightFollower
{
public:
	// The group's light byte as the pack has it (before any chain ran).
	void Begin(int base) { mBase = base; mLast = base; mDir = 0; mChanges = 0; mIdle = 0; mPeak = base; }
	// One tic with the byte now. Returns the light's intensity 0..1: how far the byte stands above the pack's value.
	double Sample(int light);
	double Intensity(int light) const;
	int Base() const { return mBase; }
	// The light changes the original made so far: a run of steps one way, ended by a turn or a pause. The niche's
	// three are +40, +30, -30.
	int Changes() const { return mChanges; }
	int Direction() const { return mDir; }      // +1 rising, -1 falling, 0 still (this tic)
	bool ChangeStarted() const { return mStarted; }
	bool ChangeEnded() const { return mEnded; }
	int Peak() const { return mPeak; }

private:
	int mBase = 0, mLast = 0, mDir = 0, mChanges = 0, mIdle = 0, mPeak = 0;
	bool mStarted = false, mEnded = false;
};

//==========================================================================
// THE FORCED TURN (0x3f) IN VR (owner Q7: "snap during the black frames")
//==========================================================================

// The original turns the player (cmd_player_rotation, roth_cmdcore.cpp PlayerRotation) and the game takes the new
// angle at once; what this decides is only when the HEADSET's view takes it. A VR view keeps its own yaw (the OpenXR
// device's doomYaw) and takes the player's again only when told (resetDoomYaw), as GZDoom's AActor::SetAngle tells it.
enum class TurnView
{
	None,        // flat: the engine's view follows the player as always
	Now,         // VR, nothing black coming: the view snaps to the new angle at once (one frame, no rotation)
	AtBlack,     // VR, a movie is queued or playing: the view takes it while the screen is black (the movie)
};
inline TurnView ForcedTurnView(bool vr, bool movieQueuedOrPlaying)
{
	if (!vr) return TurnView::None;
	return movieQueuedOrPlaying ? TurnView::AtBlack : TurnView::Now;
}
inline const char *TurnViewName(TurnView v)
{
	return v == TurnView::None ? "flat: the view follows as always" : v == TurnView::Now ? "VR: the view snaps now"
		: "VR: the view snaps while the screen is black (the movie)";
}

//==========================================================================
// THE BOOK IN BOTH HANDS, AND WHAT DROPS INTO THE PALM (owner Q8)
//==========================================================================

// The documents opened in both hands: the Book (i17; docs/VR_CHAPTER01_SHADOWS.md entry 7). The owner's word is for
// the Book.
inline bool OpensInBothHands(int item) { return item == 17; }

// While the original's examine popup shows a both-hands item: the hands hold it, the popup stands beside it; an item
// the original GIVES while it is up (the Book's first look gives the silver key, i20, through its OnInspect) shows in
// the off hand's palm from that tic until the popup closes -- it is already in the inventory, where the original put it.
// The original runs OnInspect as its popup opens (file_config.c:962-967), so the give can land in the very tic the
// popup is first seen, or the one before: the counts it compares with are those of kBookLookBack tics before the open.
constexpr int kBookLookBack = 2;

class BookView
{
public:
	enum Event { NONE = 0, OPENED = 1, GIVEN = 2, CLOSED = 4 };
	static constexpr int kMaxId = 399;
	void Reset() { mItem = 0; mPalm = 0; mHist = 0; }
	// One tic: the item whose popup is up (0 none), and a count of each item carried (`count(id)` for ids 1..maxId).
	template <class Count> int Tick(int popupItem, int maxId, Count count);
	int Item() const { return mItem; }            // the both-hands item open now, 0 none
	int Palm() const { return mPalm; }            // the item in the palm, 0 none

private:
	int mItem = 0, mPalm = 0;
	int mHist = 0;                                // tics of history kept (up to kBookLookBack + 1)
	int mMax = 0;
	int mBefore[kMaxId + 1] = {};                 // what the rise is measured from while open
	int mPast[kBookLookBack + 1][kMaxId + 1] = {};   // [0] last tic, [1] the one before, ...
};

template <class Count> int BookView::Tick(int popupItem, int maxId, Count count)
{
	int ev = NONE;
	if (maxId > kMaxId) maxId = kMaxId;
	if (maxId < 0) maxId = 0;
	int now[kMaxId + 1];
	now[0] = 0;
	for (int i = 1; i <= maxId; i++) now[i] = count(i);
	for (int i = maxId + 1; i <= kMaxId; i++) now[i] = 0;
	const int want = OpensInBothHands(popupItem) ? popupItem : 0;
	if (want != mItem)
	{
		if (mItem != 0) ev |= CLOSED;
		mItem = want;
		mPalm = 0;
		if (mItem != 0)
		{
			ev |= OPENED;
			// The counts kBookLookBack tics ago (as far back as there is history; none: now).
			const int back = mHist < kBookLookBack ? mHist - 1 : kBookLookBack - 1;
			for (int i = 0; i <= kMaxId; i++) mBefore[i] = back >= 0 ? mPast[back][i] : now[i];
		}
	}
	if (mItem != 0)
	{
		for (int i = 1; i <= maxId; i++)
		{
			if (now[i] > mBefore[i] && mPalm == 0 && i != mItem) { mPalm = i; ev |= GIVEN; }
			mBefore[i] = now[i];
		}
	}
	for (int k = kBookLookBack; k > 0; k--) for (int i = 0; i <= kMaxId; i++) mPast[k][i] = mPast[k - 1][i];
	for (int i = 0; i <= kMaxId; i++) mPast[0][i] = now[i];
	if (mHist < kBookLookBack + 1) mHist++;
	return ev;
}

//==========================================================================
// A HELD ITEM BROUGHT TO THE EYES PLAYS ITS EXAMINE (ledger row 37, the owner's X1)
//==========================================================================

// The item hand's card (the original's right-hand item, roth_held.cpp) within 15 cm of the eyes for half a second:
// the original's examine of that item, once per approach; the hand must leave past 25 cm to arm it again.
constexpr double kEyeNear = 0.15, kEyeFar = 0.25;
constexpr int kEyeHoldTics = 18;   // 0.5 s at 35 tics (17.5, rounded up)

class EyeExamine
{
public:
	void Reset() { mArmed = true; mHeld = 0; }
	// One world tic: whether the hand shows an item to examine, and its distance from the eyes (metres). True on the
	// tic the examine is due.
	bool Tick(bool haveItem, double distance)
	{
		if (distance > kEyeFar) { mArmed = true; mHeld = 0; return false; }
		if (!haveItem || !mArmed || distance > kEyeNear) { mHeld = 0; return false; }
		if (++mHeld < kEyeHoldTics) return false;
		mArmed = false;
		mHeld = 0;
		return true;
	}
	bool Armed() const { return mArmed; }
	int Held() const { return mHeld; }

private:
	bool mArmed = true;
	int mHeld = 0;
};

//==========================================================================
// WHAT EACH HAND SHOWS (owner Q12, Q2, Q8, Q1)
//==========================================================================

struct HandShows
{
	int item = 0;          // a picture of this item at the palm (0 none)
	bool lamp = false;     // the Lamp hangs from this hand
	bool book = false;     // this hand holds the open Book (with the other)
	bool match = false;    // this hand pinches the lit match
	int palm = 0;          // an item dropped into this palm (the off hand, the Book open)
};
// The item hand (the hand on the RIGHT side: the original's right-hand item) and the off hand, from what the original
// holds and what the VR layer carries. `rightItem`: the original's right-hand item (0 none); `lampHand`: the role
// holding the lamp or -1; `itemRole` / `offRole`: the roles of the item hand and the other.
void WhatHandsShow(int rightItem, int lampHand, bool matchLit, int bookItem, int palmItem, int itemRole, int otherRole,
	HandShows out[2]);

} // namespace held
} // namespace roth
