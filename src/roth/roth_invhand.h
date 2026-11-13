#pragma once
//
// roth_invhand.h -- A VR HAND ON A FLOATING PANEL: the laser, the trigger, the
// grip, the fingertip touch. Engine-free (roth_panelmap.h only), so
// tools/invhand_test and tools/invdraw_test run it without the engine.
//
// WHAT IT IS. The original works its inventory screen with a two-button mouse
// (handle_cursor_click, E/input.c:1260-1358). Each VR hand is that mouse: where
// it points is the mouse's position, and its two buttons are the original's
// two (the owner's scheme, 2026-11-13, the same as the hands' in the world):
//
//   the hand                                       the original's mouse
//   ---------------------------------------------- ------------------------------------
//   points (the index straight: PoseRule's,        the mouse is there (the hover: the
//     vr_handpose_math.h -- the other fingers        cursor cell follows it); a laser
//     may be curled by the grip)                      and a dot where it meets the picture
//   TRIGGER pulled                                 a PRIMARY click where the laser was
//                                                    when the index left it (the
//                                                    original's examine button)
//   GRIP squeezed while pointing                   a SECONDARY click at the laser (the
//                                                    original's use button)
//   FINGERTIP TOUCH: the pointing fingertip        a PRIMARY click where it touched,
//     comes onto the panel and backs off             when it backs off
//   GRIP squeezed while the fingertip touches      a SECONDARY click where it touched
//                                                    (and backing off then does nothing)
//   a click with the laser off the panel           a click off the panel: the original's
//                                                    "outside" (commit the held item, close)
//
// PRIMARY / SECONDARY are the original's: with its default button swap the
// primary is the RIGHT mouse button and examines, the secondary the LEFT and
// uses (docs/GAME_core.md 4.2: E/game_core.c:747, E/input.c:848-873). On the
// inventory the primary examines an item or takes a held one onto Adam /
// combines; the secondary takes an item onto the cursor, puts it in a hand box,
// swaps. Which hand clicks changes nothing (the original has one mouse); what a
// click does is the original's dispatch at that point (roth_invmouse.h).
//
// ONE ACTION PER PRESS, on the press edge: the trigger is pressed at 0.70 and
// let go at 0.35, the grip closed at 0.70 and opened at 0.35 (the picker's grip
// hysteresis, roth_vrpick.cpp kGripClose / kGripOpen, for both). Letting go does
// nothing. A button already down when the hand comes to the panel is not a press.
//
// THE POINT IS HELD from the moment the index leaves its straight position (it
// comes onto the trigger, or the trigger moves): the curling index swings the
// finger's own ray, so the trigger's click lands where the laser was when the
// index came down -- a mouse does not move when its button is pressed -- and the
// laser stays drawn to that point while the index is down, so the player sees
// what a pull will click -- for a second (kAimHoldTics): a hand at rest has its
// index on the trigger, and an old point is no aim. When the index straightens
// again the point stays a few tics more, while the pose eases back.
//
// A TOUCH must arrive from in front of the panel (the tip at least 4 cm in front
// first, then within 2.5 cm or through it), on the picture; one that slides off
// the picture, goes 15 cm through, or stops pointing is dropped with no click.
// A tip that crossed the panel between two tics touches where its path crossed.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_panelmap.h"

namespace roth
{

namespace invhand
{
	constexpr float  kPressOn     = 0.70f;   // the trigger pressed / the grip closed (roth_vrpick.cpp kGripClose)
	constexpr float  kPressOff    = 0.35f;   // let go / opened (kGripOpen)
	constexpr int    kSettleTics  = 8;       // the point stays put while the index straightens
	constexpr int    kAimHoldTics = 35;      // 1 s: the point held for the trigger, after the index left it
	constexpr double kTouchIn     = 0.025;   // metres: the fingertip is on the panel (the picker's kPokeIn)
	constexpr double kTouchOut    = 0.04;    // metres: backed off the panel (a touch ends; a new one may start)
	constexpr double kTouchDeep   = 0.15;    // metres: through the panel this far, it is not a touch
	constexpr double kNoHitBeam   = 0.60;    // metres: the laser's length when it meets nothing
}

// The index is straight: PoseRule's test (vr_handpose_math.h: the index is "on" with the
// trigger touched or moved at all), so the laser is up exactly while the drawn finger points.
inline bool InvHandIndexStraight(bool triggerTouch, float trigger) { return !(triggerTouch || trigger > 0.f); }

// What the hand does this tic.
struct InvHandInput
{
	bool valid = false;             // a controller (or the flat test's hand)
	float trigger = 0.f, grip = 0.f;
	bool triggerTouch = false;
	bool haveRay = false;           // the fingertip and the finger's direction are known
	bool fromFinger = false;        // ... from the drawn finger (a touch needs one)
	PanelV3 tip, dir;               // dir: unit
};

enum class InvHandHow { None, Trigger, Grip, Touch, TouchGrip, };

inline const char *InvHandHowName(InvHandHow h)
{
	switch (h)
	{
	case InvHandHow::Trigger:   return "trigger";
	case InvHandHow::Grip:      return "grip while pointing";
	case InvHandHow::Touch:     return "fingertip touch";
	case InvHandHow::TouchGrip: return "grip while the fingertip touches";
	default:                    return "none";
	}
}

// What the hand shows and does this tic.
struct InvHandOutput
{
	bool beam = false;              // draw the laser from the tip...
	PanelV3 beamEnd;                // ... to here
	bool dot = false;               // a dot where it meets the panel's picture
	PanelV3 dotAt;
	bool pointer = false;           // the hand is a mouse on the panel's plane this tic...
	int px = 0, py = 0;             // ... at this pixel
	bool onPicture = false;         // ... on the picture itself
	bool active = false;            // the index is down or the fingertip on the panel (this hand takes the pointer)
	int click = 0;                  // 0 none, 1 PRIMARY, 2 SECONDARY
	int cx = 0, cy = 0;             // the click's pixel
	InvHandHow how = InvHandHow::None;
	double tipDist = 0;             // the fingertip's distance in front of the panel (world units; < 0 behind)
};

class InvHand
{
public:
	void Reset()
	{
		pointing = wasPointing = false;
		primed = false; trigDown = false; gripDown = false;
		held = false; heldTics = 0; settle = 0;
		haveLast = false;
		touchArmed = false; inContact = false; contactActed = false;
		havePrevTip = false;
	}

	bool Pointing() const { return pointing; }
	bool InContact() const { return inContact; }
	bool IndexDown() const { return held; }

	// One tic. `upm`: world units per metre.
	InvHandOutput Tick(const PanelPlane &plane, const InvHandInput &in, double upm)
	{
		using namespace invhand;
		InvHandOutput o;
		wasPointing = pointing;
		pointing = in.valid && InvHandIndexStraight(in.triggerTouch, in.trigger);
		if (!in.valid || !in.haveRay)
		{
			Reset();
			return o;
		}

		// ---- the two buttons' press edges ----
		const bool trigWas = trigDown, gripWas = gripDown;
		if (in.trigger >= kPressOn) trigDown = true; else if (in.trigger <= kPressOff) trigDown = false;
		if (in.grip >= kPressOn) gripDown = true; else if (in.grip <= kPressOff) gripDown = false;
		const bool trigPress = primed && trigDown && !trigWas;
		const bool gripPress = primed && gripDown && !gripWas;
		primed = true;                   // a button already down on the first tic is not a press

		// ---- the laser, and the fingertip's distance from the panel ----
		PanelPoint hit;
		const bool onPlane = PanelRay(plane, in.tip, in.dir, hit);
		PanelPoint proj;
		const double d = PanelProject(plane, in.tip, proj);
		o.tipDist = d;

		// ---- the point held while the index is down ----
		if (!pointing && wasPointing && haveLast) { held = true; heldTics = 0; heldX = lastX; heldY = lastY; }
		if (pointing && held) { held = false; settle = kSettleTics; }
		if (held && ++heldTics > kAimHoldTics) { held = false; haveLast = false; }   // too long ago: no aim

		// ---- FINGERTIP TOUCH ----
		const double in_ = kTouchIn * upm, out_ = kTouchOut * upm, deep = kTouchDeep * upm;
		bool gripUsed = false;
		if (inContact)
		{
			if (!pointing || d < -deep || !proj.inside) { inContact = false; touchArmed = false; }   // dropped: no click
			else if (gripPress && !contactActed)
			{
				contactActed = true;
				gripUsed = true;
				Fire(o, 2, contactX, contactY, InvHandHow::TouchGrip);
			}
			else if (d > out_)
			{
				inContact = false;
				touchArmed = true;
				if (!contactActed) Fire(o, 1, contactX, contactY, InvHandHow::Touch);
			}
			if (inContact && gripPress) gripUsed = true;    // a grip while touching is the touch's, never the laser's
		}
		else if (pointing && in.fromFinger)
		{
			if (d > out_) touchArmed = true;
			else if (touchArmed && d <= in_)
			{
				// Where it touched: straight under the tip, or where its path
				// since the last tic went through the panel.
				PanelPoint at = proj;
				if (d < 0.0 && havePrevTip && prevD > 0.0)
				{
					const double f = prevD / (prevD - d);
					PanelLocate(plane, prevTipOnPlane + (proj.at - prevTipOnPlane) * f, at);
				}
				if (at.inside)
				{
					inContact = true;
					contactActed = false;
					contactX = at.px; contactY = at.py;
				}
				touchArmed = false;              // on the plane beside the picture, or touching: back off first
			}
		}
		else if (!pointing) touchArmed = false;
		havePrevTip = true;
		prevD = d;
		prevTipOnPlane = proj.at;

		// ---- THE TRIGGER: a PRIMARY click where the index left the laser ----
		if (trigPress && !inContact && o.click == 0)
		{
			if (held) Fire(o, 1, heldX, heldY, InvHandHow::Trigger);
			else if (pointing) Fire(o, 1, onPlane ? hit.px : PANEL_NOWHERE, onPlane ? hit.py : PANEL_NOWHERE, InvHandHow::Trigger);
		}
		// ---- THE GRIP while pointing: a SECONDARY click at the laser ----
		const bool settling = settle > 0;
		if (gripPress && !gripUsed && !inContact && pointing && o.click == 0)
		{
			if (settling) Fire(o, 2, heldX, heldY, InvHandHow::Grip);
			else Fire(o, 2, onPlane ? hit.px : PANEL_NOWHERE, onPlane ? hit.py : PANEL_NOWHERE, InvHandHow::Grip);
		}

		// ---- where this hand's mouse is ----
		if (inContact)
		{
			o.pointer = true; o.px = contactX; o.py = contactY; o.onPicture = true; o.active = true;
		}
		else if (held || settling)
		{
			o.pointer = heldX != PANEL_NOWHERE; o.px = heldX; o.py = heldY;
			o.onPicture = OnPicture(plane, heldX, heldY);
			o.active = held;
		}
		else if (pointing)
		{
			o.pointer = onPlane;
			o.px = hit.px; o.py = hit.py;
			o.onPicture = onPlane && hit.inside;
		}
		if (pointing)
		{
			// The point the trigger would click: the laser's, or nowhere (off the
			// plane); still the held one while the index straightens.
			haveLast = true;
			lastX = settling ? heldX : onPlane ? hit.px : PANEL_NOWHERE;
			lastY = settling ? heldY : onPlane ? hit.py : PANEL_NOWHERE;
		}
		if (settling && !held) settle--;

		// ---- what is drawn ----
		if (held && heldX != PANEL_NOWHERE)
		{
			// The laser to the point a pull will click.
			const PanelV3 at = PanelWorldOf(plane, heldX + 0.5, heldY + 0.5);
			o.beam = true;
			o.beamEnd = at;
			o.dot = o.onPicture;
			o.dotAt = at;
		}
		else if (pointing)
		{
			o.beam = true;
			if (onPlane)
			{
				o.beamEnd = hit.at;
				o.dot = hit.inside;
				o.dotAt = hit.at;
			}
			else o.beamEnd = in.tip + in.dir * (kNoHitBeam * upm);
		}
		return o;
	}

private:
	static bool OnPicture(const PanelPlane &p, int x, int y)
	{
		return x >= p.left && x < p.left + p.w && y >= p.top && y < p.top + p.h;
	}
	void Fire(InvHandOutput &o, int button, int x, int y, InvHandHow how)
	{
		o.click = button; o.cx = x; o.cy = y; o.how = how;
	}

	bool pointing = false, wasPointing = false;
	// the buttons
	bool primed = false, trigDown = false, gripDown = false;
	// the point held while the index is down, and after
	bool held = false;
	int heldTics = 0, settle = 0;
	int heldX = 0, heldY = 0;
	bool haveLast = false;
	int lastX = 0, lastY = 0;
	// the touch
	bool touchArmed = false, inContact = false, contactActed = false;
	int contactX = 0, contactY = 0;
	bool havePrevTip = false;
	double prevD = 0;
	PanelV3 prevTipOnPlane;
};

} // namespace roth
