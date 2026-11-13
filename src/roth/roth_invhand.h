#pragma once
//
// roth_invhand.h -- A VR HAND ON A FLOATING PANEL: the laser, the index tap, the
// fingertip touch. Engine-free (roth_panelmap.h only), so tools/invhand_test and
// tools/invdraw_test run it without the engine.
//
// WHAT IT IS. The original works its inventory screen with a two-button mouse
// (handle_cursor_click, E/input.c:1260-1358: the left button is the "primary",
// the right the "secondary", poll_mouse_input :858-868). Each VR hand is that
// mouse: where it points is the mouse's position, and its gestures are the two
// buttons. What a click does is decided by the original's code at that point
// (roth_invmouse.h), never here. Both hands work the same way; which hand made
// a click changes nothing (the original has one mouse).
//
//   the hand                                      the original's mouse
//   --------------------------------------------- ------------------------------------
//   points at the panel (the laser, a dot where  the mouse is there (the hover: the
//     it meets it)                                 cursor cell follows it)
//   INDEX TAP: the index comes onto the trigger   a PRIMARY (left) click where the
//     and lifts before 18 tics, not pulling it     laser was when the index came down
//   INDEX HOLD: the index stays on the trigger    a SECONDARY (right) click there,
//     18 tics, not pulling it                      once, when the 18th tic is reached
//   FINGERTIP TOUCH: the pointing fingertip       a PRIMARY click where it touched,
//     comes onto the panel and backs off           when it backs off
//     before 18 tics
//   FINGERTIP HOLD: it stays on the panel 18 tics a SECONDARY click where it touched
//   a tap with the laser off the panel            a click off the panel: the original's
//                                                  "outside" (commit the held item, close)
//
// The tap and its 18 tics, the 0.3 pull that makes it a shot instead, the 0.5
// grip of the pointing hand, the 2.5 cm fingertip contact: the world picker's
// own (roth_vrpick.cpp), so a hand works the panel as it works the world. The
// hold-as-secondary is new here: the world picker drops a held index (it has
// no second button to give), the panel needs the original's second button --
// a secondary click takes an item onto the cursor, puts it in a hand, swaps.
//
// THE POINT IS HELD while a tap or a touch is under way, and for a few tics
// after a tap: the index curling onto the trigger swings the finger's own ray
// (the pose moves the index 0.3 of the way to the fist, then the smoothing
// brings it back), so the click lands where the laser was when the index came
// down -- a mouse does not move when its button is pressed -- and the cursor
// does not wander with the curling finger.
//
// A TOUCH must arrive from in front of the panel (the tip at least 4 cm in front
// first, then within 2.5 cm or through it), on the picture; one that slides
// off the picture, goes 15 cm through, or stops pointing is dropped with no
// click. A tip that crossed the panel between two tics touches where its path
// crossed.
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
	constexpr int    kTapMaxTics  = 18;      // 0.51 s: the world picker's tap (roth_vrpick.cpp)
	constexpr float  kTapMaxPull  = 0.30f;   // a pull past this is a shot, not a tap
	constexpr float  kPointGrip   = 0.50f;   // the pointing hand holds the grip
	constexpr int    kSettleTics  = 8;       // the point stays put while the index straightens
	constexpr double kTouchIn     = 0.025;   // metres: the fingertip is on the panel (the picker's kPokeIn)
	constexpr double kTouchOut    = 0.04;    // metres: backed off the panel (a touch ends; a new one may start)
	constexpr double kTouchDeep   = 0.15;    // metres: through the panel this far, it is not a touch
	constexpr double kNoHitBeam   = 0.60;    // metres: the laser's length when it meets nothing
}

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

enum class InvHandHow { None, Tap, Hold, Touch, TouchHold, };

inline const char *InvHandHowName(InvHandHow h)
{
	switch (h)
	{
	case InvHandHow::Tap:       return "index tap";
	case InvHandHow::Hold:      return "index held on the trigger";
	case InvHandHow::Touch:     return "fingertip touch";
	case InvHandHow::TouchHold: return "fingertip held on the panel";
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
	bool active = false;            // a tap or a touch is under way (this hand takes the pointer)
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
		tapDown = false; tapTics = 0; tapPull = 0.f; tapLong = false; settle = 0;
		haveLast = false;
		touchArmed = false; inContact = false; contactTics = 0; contactLong = false;
		havePrevTip = false;
	}

	bool Pointing() const { return pointing; }
	bool InContact() const { return inContact; }
	bool TapDown() const { return tapDown; }

	// One tic. `upm`: world units per metre.
	InvHandOutput Tick(const PanelPlane &plane, const InvHandInput &in, double upm)
	{
		using namespace invhand;
		InvHandOutput o;
		wasPointing = pointing;
		pointing = in.valid && in.grip >= kPointGrip && !in.triggerTouch;
		if (!in.valid || !in.haveRay)
		{
			Reset();
			return o;
		}

		// ---- the laser, while pointing (or a tap is under way) ----
		PanelPoint hit;
		const bool onPlane = PanelRay(plane, in.tip, in.dir, hit);

		// ---- the fingertip's distance from the panel ----
		PanelPoint proj;
		const double d = PanelProject(plane, in.tip, proj);
		o.tipDist = d;

		// ---- INDEX TAP / HOLD ----
		const bool tapStarting = !tapDown && !inContact && wasPointing && in.triggerTouch;
		if (tapDown)
		{
			tapTics++;
			if (in.trigger > tapPull) tapPull = in.trigger;
			if (tapPull >= kTapMaxPull || in.grip < kPointGrip) { tapDown = false; settle = kSettleTics; }   // a shot, or a let-go: nothing
			else if (!in.triggerTouch)
			{
				tapDown = false;
				settle = kSettleTics;
				if (!tapLong) Fire(o, 1, tapX, tapY, InvHandHow::Tap);
			}
			else if (tapTics >= kTapMaxTics && !tapLong)
			{
				tapLong = true;
				Fire(o, 2, tapX, tapY, InvHandHow::Hold);
			}
		}
		else if (tapStarting && in.grip >= kPointGrip && in.trigger < kTapMaxPull && haveLast)
		{
			tapDown = true;
			tapTics = 0;
			tapPull = in.trigger;
			tapLong = false;
			tapX = lastX; tapY = lastY;         // where the laser was when the index came down
		}

		// ---- FINGERTIP TOUCH / HOLD ----
		const double in_ = kTouchIn * upm, out_ = kTouchOut * upm, deep = kTouchDeep * upm;
		if (inContact)
		{
			contactTics++;
			if (!pointing || d < -deep || !proj.inside) { inContact = false; touchArmed = false; }   // dropped: no click
			else if (d > out_)
			{
				inContact = false;
				touchArmed = true;
				if (!contactLong) Fire(o, 1, contactX, contactY, InvHandHow::Touch);
			}
			else if (contactTics >= kTapMaxTics && !contactLong)
			{
				contactLong = true;
				Fire(o, 2, contactX, contactY, InvHandHow::TouchHold);
			}
		}
		else if (pointing && in.fromFinger && !tapDown)
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
					contactTics = 0;
					contactLong = false;
					contactX = at.px; contactY = at.py;
				}
				touchArmed = false;              // on the plane beside the picture, or touching: back off first
			}
		}
		else if (!pointing) touchArmed = false;
		havePrevTip = true;
		prevD = d;
		prevTipOnPlane = proj.at;

		// ---- where this hand's mouse is ----
		const bool held = tapDown || settle > 0;     // the point stays where the tap took it
		if (inContact)
		{
			o.pointer = true; o.px = contactX; o.py = contactY; o.onPicture = true; o.active = true;
		}
		else if (tapDown || settle > 0)
		{
			o.pointer = tapX != PANEL_NOWHERE; o.px = tapX; o.py = tapY;
			o.onPicture = tapX >= plane.left && tapX < plane.left + plane.w && tapY >= plane.top && tapY < plane.top + plane.h;
			o.active = tapDown;
			if (settle > 0 && !tapDown) settle--;
		}
		else if (pointing)
		{
			o.pointer = onPlane;
			o.px = hit.px; o.py = hit.py;
			o.onPicture = onPlane && hit.inside;
		}
		if (pointing)
		{
			// The point a tap would click: the laser's, or nowhere (off the plane);
			// still the last tap's while the index straightens.
			haveLast = true;
			lastX = held ? tapX : onPlane ? hit.px : PANEL_NOWHERE;
			lastY = held ? tapY : onPlane ? hit.py : PANEL_NOWHERE;
		}
		else if (!tapDown) haveLast = false;

		// ---- what is drawn ----
		if (pointing || tapDown)
		{
			o.beam = true;
			if (tapDown && tapX != PANEL_NOWHERE)
			{
				// The laser to the point the click will take.
				const PanelV3 at = PanelWorldOf(plane, tapX + 0.5, tapY + 0.5);
				o.beamEnd = at;
				o.dot = o.onPicture;
				o.dotAt = at;
			}
			else if (!tapDown && onPlane)
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
	void Fire(InvHandOutput &o, int button, int x, int y, InvHandHow how)
	{
		o.click = button; o.cx = x; o.cy = y; o.how = how;
	}

	bool pointing = false, wasPointing = false;
	// the tap
	bool tapDown = false, tapLong = false;
	int tapTics = 0, settle = 0;
	float tapPull = 0.f;
	int tapX = 0, tapY = 0;
	bool haveLast = false;
	int lastX = 0, lastY = 0;
	// the touch
	bool touchArmed = false, inContact = false, contactLong = false;
	int contactTics = 0, contactX = 0, contactY = 0;
	bool havePrevTip = false;
	double prevD = 0;
	PanelV3 prevTipOnPlane;
};

} // namespace roth
