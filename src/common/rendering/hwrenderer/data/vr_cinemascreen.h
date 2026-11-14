#pragma once
//
// vr_cinemascreen.h -- RS FORK: A GAME'S CINEMA SCREEN IN VR (VRealms ledger row 35, the owner's M1: "movies on a
// world-locked screen"). While a game asks for it (VR_SetCinemaScreen), the OpenXR virtual screen -- where every
// non-level gamestate, a movie included, is drawn -- stands FIXED IN THE WORLD where the head looked when it began,
// at the game's distance and width, upright, on black: it does not follow the head (vr_overlayscreen's modes 2-5) and
// does not take the player's archived vr_overlayscreen_* settings. When the request ends the archived behaviour is
// back. Flat: nothing reads it. Engine-free (the device, vk_openxrdevice.cpp, does the placing; tools/held_test
// tests the latch and the placement).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <cmath>

struct FVRCinemaScreen
{
	bool on = false;
	float distance = 3.0f;   // metres from the head when it began
	float width = 3.2f;      // metres; the height follows the screen's picture
	float vpos = 0.0f;       // metres above the head's height
};

// Defined in hw_vrmodes.cpp. The game sets it when its movies start and clears it when they end.
void VR_SetCinemaScreen(const FVRCinemaScreen &screen);
const FVRCinemaScreen &VR_CinemaScreen();

namespace VRCinema
{

struct Vec { float x = 0, y = 0, z = 0; };

// The screen's place: `distance` ahead of the head along its YAW only (OpenXR: y up, -z ahead), `vpos` up, upright,
// its face toward the head. right x up = the normal, which points back at the head.
struct Place
{
	Vec centre, right, up, normal;
};

inline Place PlaceFor(const Vec &head, const Vec &headForward, float distance, float vpos)
{
	Place p;
	float fx = headForward.x, fz = headForward.z;
	float l = std::sqrt(fx * fx + fz * fz);
	if (l < 1e-4f) { fx = 0; fz = -1; l = 1; }
	fx /= l; fz /= l;
	p.centre = { head.x + fx * distance, head.y + vpos, head.z + fz * distance };
	p.normal = { -fx, 0, -fz };
	p.up = { 0, 1, 0 };
	p.right = { p.up.y * p.normal.z - p.up.z * p.normal.y, p.up.z * p.normal.x - p.up.x * p.normal.z, p.up.x * p.normal.y - p.up.y * p.normal.x };
	return p;
}

// The latch: placed once, on the first frame the request is on (from the head THEN), kept while it stays on, forgotten
// when it ends. Update returns true on the frame it places the screen.
class Anchor
{
public:
	bool Update(bool on, const Vec &head, const Vec &headForward, float distance, float vpos)
	{
		if (!on) { mHave = false; return false; }
		if (mHave) return false;
		mPlace = PlaceFor(head, headForward, distance, vpos);
		mHave = true;
		return true;
	}
	bool Have() const { return mHave; }
	const Place &Get() const { return mPlace; }

private:
	bool mHave = false;
	Place mPlace;
};

} // namespace VRCinema
