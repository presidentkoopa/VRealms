// tools/vrpick_test: the grip's geometry on the owner's run's own numbers (25b13e97, vrpick_flat.cfg,
// "seek door 0.08": aim (-76 3716 143), stand (-60 3716 0), palm at (-71.5 3720.3 141.9), 34 units per metre).
//  1. roth_touchgeom.h's distance from the palm to the leaf's broad side (the line x = -76 through the aim).
//  2. the grip's ray along the HAND's axis (facing the door, -x) from kGrabBack behind the palm reaches that
//     line within kGrabBack + kGrabReach -- the reach the code uses (roth_vrpick.cpp).
//  3. a ray of the same length in a direction mostly ALONG the door (what a curled index's tip-to-aim line
//     gives, the old grip ray) does not -- the failure the run logged.
#include "roth_touchgeom.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
static void check(bool ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

// Where a 2D ray from (ox, oy) along (dx, dy) crosses the line x = X, or -1.
static double crossX(double ox, double oy, double dx, double dy, double X)
{
	(void)oy;
	if (fabs(dx) < 1e-12) return -1;
	const double t = (X - ox) / dx;
	return t >= 0 ? t : -1;
}

int main()
{
	const double upm = 34.0, back = 0.08 * upm, reach = 0.20 * upm, touch = 0.10 * upm;
	const double px = -71.5, py = 3720.3;
	const roth::TouchSeg leaf{ -76.0, 3716.0 - 16.0, -76.0, 3716.0 + 16.0 };   // the broad side, 32 units of it
	double cx, cy;
	const double d = roth::TouchDistance(leaf, px, py, cx, cy);
	printf("palm to the leaf: %.2f units (%.3f m), nearest point (%.1f %.1f)\n", d, d / upm, cx, cy);
	check(fabs(d - 4.5) < 1e-9, "1. the palm is 4.5 units from the leaf's line");
	check(d > touch, "1. that is beyond the palm-touch radius (0.10 m = 3.4 units): the touch test alone does not take it");
	const double t = crossX(px - (-1.0) * back, py, -1.0, 0.0, -76.0);
	printf("hand-axis ray (-1 0) from %.1f behind the palm: crosses the leaf %.2f units along (limit %.2f)\n", back, t, back + reach);
	check(t >= 0 && t <= back + reach, "2. the grip's ray along the hand's axis reaches the leaf");
	const double ang = 80.0 * M_PI / 180.0;   // 80 degrees off the door's normal
	const double dx = -cos(ang), dy = -sin(ang);
	const double t2 = crossX(px - dx * back, py - dy * back, dx, dy, -76.0);
	printf("a ray 80 degrees off the normal: crosses %.2f units along (limit %.2f)\n", t2, back + reach);
	check(t2 < 0 || t2 > back + reach, "3. a ray mostly along the door, same length, misses it");
	printf("grip geometry: %d failure(s)\n", fails);
	return fails ? 1 : 0;
}
