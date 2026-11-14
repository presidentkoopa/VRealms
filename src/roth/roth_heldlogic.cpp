// roth_heldlogic.cpp -- see roth_heldlogic.h. Engine-free.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_heldlogic.h"

#include <algorithm>

namespace roth
{
namespace held
{

//==========================================================================
// The stroke
//==========================================================================

void StrikeDetector::Slab(int side, V3 &lo, V3 &hi)
{
	// Along the long axis: the strip's length. Across: from kStrikeInside into the box to kStrikeNear outside the
	// strip's face, on that side. Through the thickness: the strip's height, with the same inside tolerance.
	lo.x = -kBoxLength * 0.5; hi.x = kBoxLength * 0.5;
	const double face = kBoxWidth * 0.5;
	if (side > 0) { lo.y = face - kStrikeInside; hi.y = face + kStrikeNear; }
	else          { lo.y = -face - kStrikeNear; hi.y = -face + kStrikeInside; }
	lo.z = -kBoxThick * 0.5 - kStrikeInside; hi.z = kBoxThick * 0.5 + kStrikeInside;
}

bool StrikeDetector::Clip(const V3 &a, const V3 &b, const V3 &lo, const V3 &hi, double &t0, double &t1)
{
	// Liang-Barsky against an axis-aligned box.
	t0 = 0.0; t1 = 1.0;
	const double p[3] = { a.x, a.y, a.z }, q[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
	const double l[3] = { lo.x, lo.y, lo.z }, h[3] = { hi.x, hi.y, hi.z };
	for (int k = 0; k < 3; k++)
	{
		if (std::fabs(q[k]) < 1e-12)
		{
			if (p[k] < l[k] || p[k] > h[k]) return false;
			continue;
		}
		double ta = (l[k] - p[k]) / q[k], tb = (h[k] - p[k]) / q[k];
		if (ta > tb) std::swap(ta, tb);
		t0 = std::max(t0, ta);
		t1 = std::min(t1, tb);
		if (t0 > t1) return false;
	}
	return true;
}

bool StrikeDetector::Sample(const BoxFrame &box, const V3 &head, double dt)
{
	const V3 now = box.Local(head);
	if (!mHave || !(dt > 0.0))
	{
		mHave = true;
		mLast = now;
		mRun = 0; mDir = 0; mStrip = 0; mInContact = false;
		return false;
	}
	const V3 a = mLast, b = now;
	mLast = now;
	const double dAlong = b.x - a.x;
	mLastSpeed = std::fabs(dAlong) / dt;
	const int dir = dAlong > 0 ? 1 : dAlong < 0 ? -1 : 0;

	// The part of this tic's path that ran along a strip.
	double best = 0.0;
	int strip = 0;
	for (int side = -1; side <= 1; side += 2)
	{
		V3 lo, hi;
		Slab(side, lo, hi);
		double t0 = 0, t1 = 0;
		if (!Clip(a, b, lo, hi, t0, t1)) continue;
		const double along = std::fabs(dAlong) * (t1 - t0);
		if (along > best || strip == 0) { best = along; strip = side; }
	}
	mInContact = strip != 0;
	if (!mInContact || dir == 0 || mLastSpeed < kStrikeSpeed)
	{
		// Off the strip, still, or too slow: the stroke so far does not count.
		mRun = 0; mDir = 0; mStrip = 0;
		return false;
	}
	if (dir != mDir || strip != mStrip) { mRun = 0; mDir = dir; mStrip = strip; }
	mRun += best;
	if (mRun >= kStrikeRun)
	{
		mRun = 0; mDir = 0; mStrip = 0;
		return true;
	}
	return false;
}

//==========================================================================
// The match
//==========================================================================

double Match::Flicker(double s)
{
	// Three incommensurate sines, as a small flame's: 0.78..1.0.
	const double w = 0.5 * (std::sin(s * 13.1) * 0.5 + std::sin(s * 23.7 + 1.3) * 0.3 + std::sin(s * 41.3 + 2.1) * 0.2) + 0.5;
	return 0.78 + 0.22 * std::min(1.0, std::max(0.0, w));
}

//==========================================================================
// The lamp
//==========================================================================

bool LampHook::WantsGrip(int role, const V3 &palm, const V3 &bail) const
{
	if (role < 0 || role > 1) return false;
	if (mHand == role) return true;
	if (mHand != ON_HOOK) return false;
	return (palm - bail).Length() <= kLampGrabReach;
}

int LampHook::Tick(const bool valid[2], const V3 palm[2], const bool grip[2], const V3 &bail)
{
	int ev = 0;
	for (int r = 0; r < 2; r++)
	{
		const bool closed = valid[r] && grip[r];
		const bool edge = mPrimed[r] && closed && !mGripWas[r];
		if (mHand == r && (!valid[r] || !grip[r])) { mHand = ON_HOOK; ev = -1; }        // let go (or lost): to the hook
		else if (mHand == ON_HOOK && edge && (palm[r] - bail).Length() <= kLampGrabReach) { mHand = r; ev = +1; }
		mGripWas[r] = closed;
		mPrimed[r] = valid[r];
	}
	return ev;
}

V3 LampHook::HookPoint(const V3 &feet, const V3 &forward, const V3 &right, const V3 &up, double eyeHeight, int offRight)
{
	return feet + up * (eyeHeight * kHookHeight) + right * (kHookSide * (offRight >= 0 ? 1.0 : -1.0)) + forward * kHookForward;
}

//==========================================================================
// The light follower
//==========================================================================

double LightFollower::Intensity(int light) const
{
	const double v = (double)(light - mBase) / (double)kFollowFull;
	return v < 0 ? 0.0 : v > 1 ? 1.0 : v;
}

double LightFollower::Sample(int light)
{
	mStarted = mEnded = false;
	const int d = light - mLast;
	const int dir = d > 0 ? 1 : d < 0 ? -1 : 0;
	if (dir != 0)
	{
		if (dir != mDir) { if (mDir != 0) mEnded = true; mChanges++; mStarted = true; }
		mIdle = 0;
	}
	else if (mDir != 0)
	{
		// One still tic ends a run: a ramp steps every tic until it is done (tick_change_lighting).
		mEnded = true;
	}
	mDir = dir;
	mLast = light;
	if (light > mPeak) mPeak = light;
	return Intensity(light);
}

//==========================================================================
// The hands
//==========================================================================

void WhatHandsShow(int rightItem, int lampHand, bool matchLit, int bookItem, int palmItem, int itemRole, int otherRole,
	HandShows out[2])
{
	out[0] = HandShows();
	out[1] = HandShows();
	if (itemRole < 0 || itemRole > 1 || otherRole < 0 || otherRole > 1 || itemRole == otherRole) return;
	if (bookItem != 0)
	{
		// The Book open: both hands hold it; what the original gives meanwhile lies in the off hand's palm.
		out[itemRole].book = out[otherRole].book = true;
		out[otherRole].palm = palmItem;
		return;
	}
	// The lamp hangs from the hand that took it; that hand shows nothing else.
	if (lampHand == itemRole || lampHand == otherRole) out[lampHand].lamp = true;
	// The match burns in the off hand (owner Q1), unless that hand took the lamp (then it was put out).
	if (matchLit && !out[otherRole].lamp) out[otherRole].match = true;
	// The original's right-hand item in the item hand (owner Q12). The Lamp selected into the right hand IS the lamp:
	// it hangs from that hand (the hook is empty) -- roth_held.cpp draws it so.
	if (!out[itemRole].lamp && rightItem > 0) out[itemRole].item = rightItem;
}

} // namespace held
} // namespace roth
