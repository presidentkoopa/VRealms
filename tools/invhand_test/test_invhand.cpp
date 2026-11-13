// tools/invhand_test -- the VR hands on the inventory panel, engine-free.
//
//  M. THE MAP (src/roth/roth_panelmap.h): a point on the floating panel -> the original's
//     screen pixel, and back.
//     M1  every pixel of the 0x120 x 0x50 panel: its centre in the world -> the same pixel,
//         dropped straight on the plane and along a ray from an eye in front, for several
//         panel yaws, tilts (bb_tiltbias), scales (bb_scale) and bb_flipu;
//     M2  the same pixel as the engine's PanelPixel computed it at 471eb9d7 (its code is
//         copied below as it stood: roth_interface.cpp:837-857), so the vanilla beam lands
//         where it always did;
//     M3  world -> pixel -> world: within half a pixel of where it started;
//     M4  the corners and the outside: u = 0 is x 0, u just under 1 is x 0x11f, left of the
//         panel is negative, a ray parallel to the plane or pointing away meets nothing;
//     M5  the fingertip's signed distance in front of the plane.
//  G. THE HAND (src/roth/roth_invhand.h): scripted tics of a hand. Trigger = the original's
//     PRIMARY, grip = its SECONDARY (the owner's scheme).
//     G1  pointing (the index straight, the grip open or closed): the mouse is where the laser
//         meets the panel; a dot on the picture only; the index on the trigger holds the point;
//     G2  the trigger: one PRIMARY per press (0.70 on, 0.35 off) where the laser was when the
//         index came down, though the curling finger swings the ray; letting go does nothing;
//         the point is held while the index straightens, and for at most 35 tics while it is down;
//         an instant pull (touch and pull in one tic) and a touch-then-pull both click. The
//         harness reads the finger only when InvHand::WantsRay says so, as the engine does (the
//         owner's PC run of 983ef7b8: the engine skipped the ray on the tic the index left, and the
//         trigger clicked nothing; these cases failed under that gate);
//     G3  a controller with no trigger touch: the same;
//     G4  the grip while pointing: one SECONDARY per squeeze at the laser; not while the index is
//         on the trigger; a grip already closed when the hand arrives is no press;
//     G5  a click with the laser off the panel or away from its plane: a click off the panel
//         (PANEL_NOWHERE when the ray meets no plane);
//     G6  a fingertip touch from in front: PRIMARY where it touched, when it backs off, however
//         long it rested (no hold-as-secondary);
//     G7  the grip while the fingertip touches: one SECONDARY there, and nothing on backing off;
//     G8  a fast push through the panel between two tics: touches where its path crossed;
//     G9  touches that are not: from behind, beside the picture, sliding off it, 15 cm
//         through, a hand that stops pointing;
//     G10 no input (no controller): nothing, and the state is cleared.
#include "roth_panelmap.h"
#include "roth_invhand.h"
#include <stdio.h>
#include <math.h>
#include <vector>

using namespace roth;

static int fails = 0, checks = 0;
static void check(bool ok, const char *what)
{
	checks++;
	if (!ok) { fails++; printf("FAIL  %s\n", what); }
}

// BillboardBasis (src/g_levellocals.h:508-610) for a BBF_FIXED billboard with no
// roll and no hand lock: the axes the renderer draws the panel with.
static void Basis(double yawDeg, double tiltDeg, double scale, double width, double height,
	PanelV3 &right, PanelV3 &up, PanelV3 &normal, double &hw, double &hh)
{
	const double DEG2RAD = 0.01745329251994329576923690768489;
	const double yawRad = yawDeg * DEG2RAD, tiltRad = tiltDeg * DEG2RAD;
	const double cy = cos(yawRad), sy = sin(yawRad), ct = cos(tiltRad), st = sin(tiltRad);
	right = PanelV3(-sy, cy, 0.0);
	up = PanelV3(-cy * st, -sy * st, ct);
	normal = PanelV3(right.y * up.z - right.z * up.y, right.z * up.x - right.x * up.z, right.x * up.y - right.y * up.x);
	const double g = scale > 0.01 ? scale : 0.01;
	hw = width * 0.5 * g;
	hh = height * 0.5 * g;
}

// The inventory panel as roth_interface.cpp stands it: AddWorldPicture at PXW = 0.1 * 1.4
// map units per pixel, centred (cx - 312, 280 - cy) * PXW from the anchor, yaw = the
// player's + 180, so its face points back at him.
struct Setup { double playerYaw, tilt, scale; bool flipu; };
static PanelPlane Panel(const Setup &s, PanelV3 &eye, PanelV3 &normalOut)
{
	const double PXW = 0.1 * 1.4;
	const double yr = s.playerYaw * M_PI / 180.0;
	eye = PanelV3(1234.5, -987.25, 56.0);
	const PanelV3 fwd(cos(yr), sin(yr), 0.0);
	const PanelV3 anchor = eye + fwd * 44.0 - PanelV3(0, 0, 4);
	const double bbYaw = s.playerYaw + 180.0;
	PanelPlane p;
	PanelV3 normal;
	Basis(bbYaw, s.tilt, s.scale, 0x120 * PXW, 0x50 * PXW, p.right, p.up, normal, p.halfw, p.halfh);
	const double cx = (640 - 0x130) / 2 + 0 + 0x120 * 0.5, cy = 480 / 2 + 0 + 0x50 * 0.5;
	const double byr = bbYaw * M_PI / 180.0;
	const PanelV3 bright(-sin(byr), cos(byr), 0.0);
	p.centre = anchor + bright * ((cx - 312) * PXW) + PanelV3(0, 0, (280 - cy) * PXW);
	p.front = (eye - p.centre).Dot(normal) >= 0 ? normal : normal * -1.0;
	p.flipu = s.flipu;
	p.left = 0; p.top = 0; p.w = 0x120; p.h = 0x50;
	normalOut = normal;
	return p;
}

// roth_interface.cpp:837-857 at 471eb9d7 (PanelPixel), the arithmetic as it stood.
static bool OldPanelPixel(const PanelPlane &p, const PanelV3 &normal, const PanelV3 &start, const PanelV3 &dir, int &px, int &py)
{
	const double hw = p.halfw, hh = p.halfh;
	const double denom = normal.Dot(dir);
	if (fabs(denom) < 1e-9 || hw <= 0 || hh <= 0) return false;
	const double t = normal.Dot(p.centre - start) / denom;
	if (t <= 0.0) return false;
	const PanelV3 rel = (start + dir * t) - p.centre;
	double u = (rel.Dot(p.right) / hw + 1.0) * 0.5;
	const double v = (1.0 - rel.Dot(p.up) / hh) * 0.5;
	if (p.flipu) u = 1.0 - u;
	px = p.left + (int)floor(u * p.w);
	py = p.top + (int)floor(v * p.h);
	return true;
}

static PanelV3 Unit(const PanelV3 &v) { const double l = v.Length(); return v * (1.0 / l); }

static void TestMap()
{
	const Setup setups[] = {
		{ 0.0, 0.0, 1.0, false }, { 90.0, 0.0, 1.0, false }, { 37.25, 0.0, 1.0, false }, { 211.0, 0.0, 1.0, true },
		{ -63.0, 12.0, 1.0, false }, { 145.0, -20.0, 1.3, false }, { 300.0, 5.0, 0.8, true },
	};
	long m1 = 0, m1bad = 0, m2 = 0, m2bad = 0, m3 = 0, m3bad = 0;
	double worst = 0;
	for (const Setup &s : setups)
	{
		PanelV3 eye, normal;
		const PanelPlane p = Panel(s, eye, normal);
		// eyes: straight in front, high and to the side, low and to the other side (a hand)
		const PanelV3 eyes[3] = { eye, p.centre + p.front * 30.0 + p.right * 15.0 + p.up * 9.0,
			p.centre + p.front * 6.0 - p.right * 22.0 - p.up * 7.0 };
		for (int y = 0; y < 0x50; y++)
			for (int x = 0; x < 0x120; x++)
			{
				const PanelV3 w = PanelWorldOf(p, x + 0.5, y + 0.5);
				PanelPoint pt;
				PanelProject(p, w, pt);
				m1++;
				if (pt.px != x || pt.py != y || !pt.inside) m1bad++;
				for (const PanelV3 &e : eyes)
				{
					const PanelV3 dir = Unit(w - e);
					PanelPoint r;
					m1++;
					if (!PanelRay(p, e, dir, r) || r.px != x || r.py != y || !r.inside) m1bad++;
					int ox = 0, oy = 0;
					m2++;
					if (!OldPanelPixel(p, normal, e, dir, ox, oy) || ox != r.px || oy != r.py) m2bad++;
				}
				// M3: a point anywhere in the pixel -> the pixel -> its centre, within half a pixel
				const double fx = x + 0.13 + 0.74 * ((x * 7 + y * 3) % 11) / 10.0, fy = y + 0.07 + 0.86 * ((x + y * 5) % 13) / 12.0;
				const PanelV3 q = PanelWorldOf(p, fx, fy);
				PanelPoint qp;
				PanelProject(p, q, qp);
				const PanelV3 back = PanelWorldOf(p, qp.px + 0.5, qp.py + 0.5);
				const double pxw = 2.0 * p.halfw / p.w, pxh = 2.0 * p.halfh / p.h;
				const double du = fabs((back - q).Dot(p.right)) / pxw, dv = fabs((back - q).Dot(p.up)) / pxh;
				m3++;
				if (qp.px != x || qp.py != y || du > 0.5 + 1e-9 || dv > 0.5 + 1e-9) m3bad++;
				if (du > worst) worst = du;
				if (dv > worst) worst = dv;
			}
		// M4: corners and outside
		PanelPoint c;
		PanelProject(p, PanelWorldOf(p, 1e-6, 1e-6), c);
		check(c.px == 0 && c.py == 0 && c.inside, "M4 just inside the top left corner is pixel (0, 0)");
		PanelProject(p, PanelWorldOf(p, 0x120 - 1e-6, 0x50 - 1e-6), c);
		check(c.px == 0x11f && c.py == 0x4f && c.inside, "M4 just inside the bottom right corner is pixel (0x11f, 0x4f)");
		PanelProject(p, PanelWorldOf(p, -0.5, 10.5), c);
		check(c.px == -1 && !c.inside, "M4 half a pixel left of the panel is x -1, off the picture");
		PanelProject(p, PanelWorldOf(p, 0x120 + 0.5, 0x50 + 3.5), c);
		check(c.px == 0x120 && c.py == 0x53 && !c.inside, "M4 past the bottom right is (0x120, 0x53), off the picture");
		PanelProject(p, PanelWorldOf(p, -40.5, -20.5), c);
		check(c.px == -41 && c.py == -21, "M4 far outside keeps counting pixels");
		PanelPoint r;
		check(!PanelRay(p, eye, p.right, r) && r.px == PANEL_NOWHERE && !r.onPlane, "M4 a ray along the plane meets nothing (PANEL_NOWHERE)");
		check(!PanelRay(p, eye, Unit(eye - p.centre), r) && r.px == PANEL_NOWHERE, "M4 a ray pointing away meets nothing");
		// M5: signed distance
		PanelPoint pr;
		const double d1 = PanelProject(p, PanelWorldOf(p, 100.5, 20.5) + p.front * 2.5, pr);
		check(fabs(d1 - 2.5) < 1e-9 && pr.px == 100 && pr.py == 20, "M5 a tip 2.5 units in front: +2.5, over pixel (100, 20)");
		const double d2 = PanelProject(p, PanelWorldOf(p, 100.5, 20.5) - p.front * 1.0, pr);
		check(fabs(d2 + 1.0) < 1e-9 && pr.px == 100, "M5 a tip 1 unit through: -1");
		check((eye - p.centre).Dot(p.front) > 0, "M5 the front faces the player's eye");
	}
	char buf[200];
	snprintf(buf, sizeof buf, "M1 every pixel's centre back to itself (%ld cases, %ld wrong)", m1, m1bad);
	check(m1bad == 0, buf); printf("%s\n", buf);
	snprintf(buf, sizeof buf, "M2 the same pixel as PanelPixel at 471eb9d7 (%ld rays, %ld differ)", m2, m2bad);
	check(m2bad == 0, buf); printf("%s\n", buf);
	snprintf(buf, sizeof buf, "M3 world -> pixel -> world within half a pixel (%ld points, %ld out; worst %.4f px)", m3, m3bad, worst);
	check(m3bad == 0, buf); printf("%s\n", buf);
}

// ---- G: the hand ----
struct Run
{
	PanelPlane p;
	InvHand h;
	double upm = 107.4;
	std::vector<InvHandOutput> outs;
	InvHandInput in;
	Run()
	{
		Setup s{ 30.0, 0.0, 1.0, false };
		PanelV3 eye, normal;
		p = Panel(s, eye, normal);
		in.valid = true; in.haveRay = true; in.fromFinger = true;
	}
	// The hand pointing (the index straight, off the trigger; the grip open) from `tip` at screen point (x, y).
	void PointAt(double x, double y, double metres = 0.35, double side = 0.1)
	{
		const PanelV3 at = PanelWorldOf(p, x, y);
		in.tip = at + p.front * (metres * upm) + p.right * (side * upm);
		in.dir = Unit(at - in.tip);
		in.grip = 0.f; in.trigger = 0.f; in.triggerTouch = false;
	}
	// A fingertip `metres` in front of screen point (x, y), the finger along the normal.
	void TipAt(double x, double y, double metres)
	{
		in.tip = PanelWorldOf(p, x, y) + p.front * (metres * upm);
		in.dir = p.front * -1.0;
		in.trigger = 0.f; in.triggerTouch = false;
	}
	// As the engine does (roth_vrpick.cpp TickPanelHand): the finger is read only
	// when InvHand::WantsRay says so; otherwise the tic has no ray.
	InvHandOutput Tic()
	{
		InvHandInput t = in;
		t.haveRay = in.haveRay && h.WantsRay(in.valid, in.triggerTouch, in.trigger);
		InvHandOutput o = h.Tick(p, t, upm);
		outs.push_back(o);
		return o;
	}
	int Clicks(int *button = nullptr, int *x = nullptr, int *y = nullptr, InvHandHow *how = nullptr) const
	{
		int n = 0;
		for (const InvHandOutput &o : outs)
			if (o.click) { n++; if (button) *button = o.click; if (x) *x = o.cx; if (y) *y = o.cy; if (how) *how = o.how; }
		return n;
	}
	// The index onto the trigger (curling: its ray swings down), then pulled to `pull`.
	void IndexDown(float pull) { in.triggerTouch = true; in.trigger = pull; in.dir = Unit(in.dir - p.up * 0.5); }
};

static void TestHand()
{
	{   // G1
		Run r;
		r.PointAt(130.5, 22.5);
		InvHandOutput o = r.Tic();
		check(o.pointer && o.px == 130 && o.py == 22 && o.onPicture && o.dot && o.beam, "G1 pointing: the mouse at (130, 22), a dot, the laser");
		check(fabs((o.beamEnd - PanelWorldOf(r.p, 130.5, 22.5)).Length()) < 1e-6, "G1 the laser ends on the panel");
		r.in.grip = 0.9f; o = r.Tic();             // the other fingers curled by the grip: still pointing
		check(r.Clicks() == 1 && o.beam && r.h.Pointing(), "G1 the index straight with the grip held is still pointing (and the squeeze clicked: G4)");
		Run q;
		q.PointAt(-30.5, 22.5);
		o = q.Tic();
		check(o.pointer && o.px == -31 && !o.onPicture && !o.dot && o.beam, "G1 beside the panel: the mouse off the picture, no dot");
		q.Tic(); q.Tic();
		check(q.Clicks() == 0, "G1 pointing alone clicks nothing");
		q.in.triggerTouch = true; o = q.Tic();
		check(!q.h.Pointing() && q.h.IndexDown(), "G1 the index onto the trigger: not pointing, the point held");
	}
	{   // G2
		Run r;
		r.PointAt(130.5, 22.5);
		r.Tic(); r.Tic();
		r.IndexDown(0.f);                          // the index onto the trigger: the ray swings
		InvHandOutput o = r.Tic();
		check(o.pointer && o.px == 130 && o.py == 22 && o.active && o.beam && o.dot, "G2 index down: the mouse and the laser held at (130, 22)");
		r.in.trigger = 0.5f; r.Tic();
		check(r.Clicks() == 0, "G2 pulled to 0.5: no click (pressed at 0.70)");
		r.in.trigger = 0.75f; r.Tic();
		int b = 0, x = 0, y = 0; InvHandHow how = InvHandHow::None;
		check(r.Clicks(&b, &x, &y, &how) == 1 && b == 1 && x == 130 && y == 22 && how == InvHandHow::Trigger, "G2 the pull past 0.70: one PRIMARY click at (130, 22)");
		r.in.trigger = 1.f; r.Tic(); r.in.trigger = 0.5f; r.Tic(); r.in.trigger = 0.8f; r.Tic();
		check(r.Clicks() == 1, "G2 held, eased to 0.5 and pulled again (no let-go below 0.35): no second click");
		r.in.trigger = 0.2f; r.Tic(); r.in.trigger = 0.9f; r.Tic();
		check(r.Clicks(&b, &x, &y) == 2 && b == 1 && x == 130, "G2 let go below 0.35 and pulled again: a second PRIMARY there");
		r.in.trigger = 0.f; r.Tic();
		check(r.Clicks() == 2, "G2 letting go does nothing");
		r.in.triggerTouch = false; r.in.dir = Unit(PanelWorldOf(r.p, 130.5, 22.5) - r.in.tip);
		r.PointAt(200.5, 40.5);                    // the index straightens, the hand already elsewhere
		bool held = true;
		for (int i = 0; i < invhand::kSettleTics; i++) { o = r.Tic(); held = held && o.px == 130 && o.py == 22; }
		check(held, "G2 the point is held while the index straightens");
		o = r.Tic();
		check(o.px == 200 && o.py == 40, "G2 then the mouse follows the laser again");
	}
	{   // G2a the instant pull: the index straight one tic, touching and pulled the next
		Run r;
		r.PointAt(199.5, 29.5);
		r.Tic(); r.Tic();
		r.IndexDown(1.f);                          // 0 -> 1 in one tic (the flat test's "0 1 1 1")
		r.Tic();
		int b = 0, x = 0, y = 0;
		check(r.Clicks(&b, &x, &y) == 1 && b == 1 && x == 199 && y == 29, "G2 an instant pull (touch and pull in one tic): one PRIMARY at (199, 29)");
		Run q;                                     // the real index: touch first, then the pull some tics later
		q.PointAt(199.5, 29.5);
		q.Tic(); q.Tic();
		q.IndexDown(0.f); q.Tic();
		for (int i = 0; i < 10; i++) q.Tic();
		q.in.trigger = 1.f; q.Tic();
		check(q.Clicks(&b, &x, &y) == 1 && b == 1 && x == 199 && y == 29, "G2 touch first, pull 11 tics later: one PRIMARY at (199, 29)");
	}
	{   // G2b the held point lasts kAimHoldTics
		Run r;
		r.PointAt(130.5, 22.5);
		r.Tic();
		r.IndexDown(0.f);
		InvHandOutput o;
		for (int i = 0; i < invhand::kAimHoldTics; i++) o = r.Tic();
		check(o.beam && o.px == 130 && r.h.IndexDown(), "G2 the index down 35 tics: the point still held, the laser to it");
		o = r.Tic();
		check(!o.beam && !o.pointer && !r.h.IndexDown(), "G2 a tic later: the point is dropped, no laser (a hand at rest)");
		r.in.trigger = 1.f; r.Tic();
		check(r.Clicks() == 0, "G2 a pull after that: no click (no aim)");
	}
	{   // G3 no touch sensor: the trigger's own travel takes the index off
		Run r;
		r.PointAt(100.5, 30.5);
		r.Tic();
		r.in.trigger = 0.3f; r.in.dir = Unit(r.in.dir - r.p.up * 0.5); r.Tic();
		r.in.trigger = 0.9f; r.Tic();
		int b = 0, x = 0, y = 0;
		check(r.Clicks(&b, &x, &y) == 1 && b == 1 && x == 100 && y == 30, "G3 a controller with no trigger touch: the pull's PRIMARY lands where the laser was");
	}
	{   // G4 the grip while pointing
		Run r;
		r.PointAt(160.5, 25.5);
		r.Tic();
		r.in.grip = 0.5f; r.Tic();
		check(r.Clicks() == 0, "G4 the grip at 0.5: no click (closed at 0.70)");
		r.in.grip = 0.8f; r.Tic();
		int b = 0, x = 0, y = 0; InvHandHow how;
		check(r.Clicks(&b, &x, &y, &how) == 1 && b == 2 && x == 160 && y == 25 && how == InvHandHow::Grip, "G4 the squeeze: one SECONDARY at (160, 25)");
		for (int i = 0; i < 30; i++) r.Tic();
		r.in.grip = 0.f; r.Tic();
		check(r.Clicks() == 1, "G4 held 30 tics, then let go: nothing more");
		r.in.triggerTouch = true; r.Tic();         // the index down, then the grip: not pointing, nothing
		r.in.grip = 1.f; r.Tic();
		check(r.Clicks() == 1, "G4 the grip with the index on the trigger (not pointing): no click");
		Run q;                                     // a grip already closed when the hand arrives is not a press
		q.PointAt(160.5, 25.5); q.in.grip = 1.f;
		q.Tic(); q.Tic();
		check(q.Clicks() == 0, "G4 a grip already closed on the first tic: no click");
	}
	{   // G5
		Run r;
		r.PointAt(-60.5, 120.5);
		r.Tic();
		r.IndexDown(0.f); r.Tic();
		r.in.trigger = 1.f; r.Tic();
		int b = 0, x = 0, y = 0;
		check(r.Clicks(&b, &x, &y) == 1 && b == 1 && x == -61 && y == 120, "G5 the trigger with the laser beside the panel: a PRIMARY at (-61, 120), off it");
		Run q;
		q.PointAt(100.5, 30.5);
		q.in.dir = q.p.front;                      // pointing back, away from the plane
		InvHandOutput o = q.Tic();
		check(!o.pointer && o.beam && !o.dot, "G5 pointing away: no mouse on the panel, a short laser");
		q.in.grip = 1.f; q.Tic();
		check(q.Clicks(&b, &x, &y) == 1 && b == 2 && x == PANEL_NOWHERE && y == PANEL_NOWHERE, "G5 the grip pointing away: a SECONDARY nowhere (off the panel)");
	}
	{   // G6 touch = PRIMARY on backing off
		Run r;
		const double xs[] = { 0.10, 0.07, 0.05, 0.03, 0.015 };
		for (double m : xs) { r.TipAt(160.5, 25.5, m); r.Tic(); }
		check(r.h.InContact(), "G6 the tip 1.5 cm in front: on the panel");
		r.TipAt(161.5, 26.5, 0.005); r.Tic();
		for (int i = 0; i < 40; i++) r.Tic();      // resting on it: nothing (no hold-as-secondary any more)
		r.TipAt(161.5, 26.5, 0.03); r.Tic();
		check(r.Clicks() == 0, "G6 resting on the panel 40 tics, still within 4 cm: no click");
		r.TipAt(161.5, 26.5, 0.06);
		r.Tic();
		int b = 0, x = 0, y = 0; InvHandHow how;
		check(r.Clicks(&b, &x, &y, &how) == 1 && b == 1 && x == 160 && y == 25 && how == InvHandHow::Touch, "G6 backing off: one PRIMARY at (160, 25), where it touched");
		for (double m : xs) { r.TipAt(40.5, 60.5, m); r.Tic(); }
		r.TipAt(40.5, 60.5, 0.1); r.Tic();
		check(r.Clicks(&b, &x, &y) == 2 && x == 40 && y == 60, "G6 a second touch: a second click at (40, 60)");
	}
	{   // G7 grip while touching = SECONDARY
		Run r;
		r.TipAt(200.5, 50.5, 0.10); r.Tic();
		r.TipAt(200.5, 50.5, 0.0); r.Tic();
		r.in.grip = 0.9f; r.Tic();
		int b = 0, x = 0, y = 0; InvHandHow how;
		check(r.Clicks(&b, &x, &y, &how) == 1 && b == 2 && x == 200 && y == 50 && how == InvHandHow::TouchGrip, "G7 the grip while touching: one SECONDARY at (200, 50)");
		for (int i = 0; i < 10; i++) r.Tic();
		r.TipAt(200.5, 50.5, 0.10); r.in.grip = 0.9f; r.Tic();
		r.in.grip = 0.f; r.Tic();
		check(r.Clicks() == 1, "G7 backing off after it, then opening the grip: nothing more");
	}
	{   // G8
		Run r;
		r.TipAt(80.5, 40.5, 0.10); r.Tic();
		const PanelV3 b = PanelWorldOf(r.p, 98.5, 40.5) - r.p.front * (0.08 * r.upm);
		r.in.tip = b; r.Tic();
		check(r.h.InContact(), "G8 a fast push through: on the panel");
		r.TipAt(98.5, 40.5, 0.10); r.Tic();
		int bt = 0, x = 0, y = 0;
		check(r.Clicks(&bt, &x, &y) == 1 && bt == 1 && x == 90 && y == 40, "G8 touched where the path crossed (90, 40), not where the tip ended (98)");
	}
	{   // G9
		Run r;
		r.TipAt(80.5, 40.5, -0.05); r.Tic();
		r.TipAt(80.5, 40.5, 0.0); r.Tic();
		r.TipAt(80.5, 40.5, 0.10); r.Tic();
		check(r.Clicks() == 0, "G9 from behind: no touch");
		Run q;
		q.TipAt(-20.5, 40.5, 0.10); q.Tic();
		q.TipAt(-20.5, 40.5, 0.0); q.Tic();
		q.TipAt(5.5, 40.5, 0.0); q.Tic();
		q.TipAt(5.5, 40.5, 0.10); q.Tic();
		check(q.Clicks() == 0, "G9 beside the picture, then sliding onto it: no touch");
		Run s;
		s.TipAt(280.5, 40.5, 0.10); s.Tic();
		s.TipAt(280.5, 40.5, 0.0); s.Tic();
		s.TipAt(300.5, 40.5, 0.0); s.Tic();
		s.TipAt(300.5, 40.5, 0.10); s.Tic();
		check(s.Clicks() == 0, "G9 sliding off the picture: dropped");
		Run t;
		t.TipAt(80.5, 40.5, 0.10); t.Tic();
		t.TipAt(80.5, 40.5, 0.0); t.Tic();
		t.TipAt(80.5, 40.5, -0.2); t.Tic();
		t.TipAt(80.5, 40.5, 0.10); t.Tic();
		check(t.Clicks() == 0, "G9 15 cm through: dropped");
		Run u;
		u.TipAt(80.5, 40.5, 0.10); u.Tic();
		u.TipAt(80.5, 40.5, 0.0); u.Tic();
		u.in.triggerTouch = true; u.Tic();          // the index leaves the panel for the trigger
		u.TipAt(80.5, 40.5, 0.10); u.Tic();
		check(u.Clicks() == 0, "G9 the hand stops pointing: dropped");
		Run v;
		v.TipAt(80.5, 40.5, 0.10); v.in.fromFinger = false; v.Tic();
		v.TipAt(80.5, 40.5, 0.0); v.in.fromFinger = false; v.Tic();
		v.TipAt(80.5, 40.5, 0.10); v.in.fromFinger = false; v.Tic();
		check(v.Clicks() == 0, "G9 no drawn finger (the aim pose): no touch");
	}
	{   // G10
		Run r;
		r.PointAt(100.5, 30.5); r.Tic();
		r.IndexDown(0.f); r.Tic();
		r.in.valid = false;
		InvHandOutput o = r.Tic();
		check(!o.beam && !o.pointer && !o.click && !r.h.IndexDown(), "G10 the controller gone: nothing shown, the held point dropped");
		r.in.valid = true; r.in.trigger = 1.f; r.Tic();
		check(r.Clicks() == 0, "G10 and a trigger already pulled when it comes back is no click");
	}
}

int main()
{
	TestMap();
	TestHand();
	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
	return fails ? 1 : 0;
}
