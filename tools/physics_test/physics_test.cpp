// The engine-free test of roth_physics_core (docs/VR_PHYSICS.md): a floor and a wall, an item that falls and rests,
// a hand stopped by the wall, a hand that pushes an item, a throw, a held item that meets the wall, the model angles.
#include "roth_physics_core.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace roth::phys;

static int gPass = 0, gFail = 0;
static void Check(bool ok, const char *what, double v = NAN)
{
	if (ok) gPass++; else gFail++;
	if (std::isnan(v)) printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	else printf("%s %s (%.4f)\n", ok ? "ok  " : "FAIL", what, v);
}

static void Quad(std::vector<float> &t, V3 a, V3 b, V3 c, V3 d)
{
	for (V3 v : { a, b, c, a, c, d }) { t.push_back((float)v.x); t.push_back((float)v.y); t.push_back((float)v.z); }
}

static std::vector<V3> Box(double hx, double hy, double hz)
{
	std::vector<V3> p;
	for (int i = 0; i < 8; i++) p.push_back(V3(i & 1 ? hx : -hx, i & 2 ? hy : -hy, i & 4 ? hz : -hz));
	return p;
}

static Q AxisAngle(V3 axis, double deg)
{
	const double a = deg * 3.14159265358979 / 360.0, l = axis.Length();
	return Q(axis.x / l * std::sin(a), axis.y / l * std::sin(a), axis.z / l * std::sin(a), std::cos(a));
}

static void Room(World &w)
{
	std::vector<float> floor, wall;
	Quad(floor, V3(-5, -5, 0), V3(5, -5, 0), V3(5, 5, 0), V3(-5, 5, 0));
	Quad(wall, V3(1, -5, 0), V3(1, 5, 0), V3(1, 5, 3), V3(1, -5, 3));
	w.SetStatic(1, floor);
	w.SetStatic(2, wall);
}

int main()
{
	const double dt = 1.0 / 35.0;
	{
		World w;
		Check(w.Ok(), "the world is up");
		Room(w);
		Check(w.StaticTriangles() == 4, "the room: four triangles");
		ItemShape s;
		s.hull = Box(0.05, 0.05, 0.05);
		const Id it = w.AddItem(s, Pose{ V3(0, 0, 1), AxisAngle(V3(1, 1, 0), 30) }, V3(), V3());
		for (int i = 0; i < 35 * 4; i++) w.Step(dt);
		Pose p;
		w.PoseOf(it, p);
		Check(std::fabs(p.p.z - 0.05) < 0.01, "a 10 cm box dropped from 1 m rests on the floor: its centre 5 cm up", p.p.z);
		Check(w.Resting(it), "and has gone to sleep");
		double d = -1;
		Check(w.ItemNear(p.p + V3(0.08, 0, 0), 0.06, &d) == it, "a hand 3 cm from its side finds it", d);
		Check(w.ItemNear(p.p + V3(0.2, 0, 0), 0.06) == kNone, "15 cm away finds nothing");
		// a throw
		const Id th = w.AddItem(s, Pose{ V3(-3, 0, 1.2), Q() }, V3(4, 0, 2), V3(0, 5, 0));
		for (int i = 0; i < 35 * 3; i++) w.Step(dt);
		w.PoseOf(th, p);
		Check(p.p.x > -1.6 && p.p.x < 1.0 && p.p.z < 0.2, "thrown at 4 m/s forward, 2 up: it lands well ahead, on the floor, short of the wall", p.p.x);
	}
	{
		World w;
		Room(w);
		HandShape palm;
		const Id h = w.AddHand(palm, Pose{ V3(0.6, 0, 1), Q() });
		// drive the hand from x 0.6 to 1.3 (behind the wall) over half a second, then hold the target there
		double hitMax = 0;
		for (int i = 0; i < 35; i++)
		{
			const double t = std::min(1.0, i / 17.0);
			w.DriveHand(h, Pose{ V3(0.6 + 0.7 * t, 0, 1), Q() }, dt);
			w.Step(dt);
			hitMax = std::max(hitMax, w.HandHit(h));
		}
		Pose hp;
		w.HandPose(h, hp);
		const double front = hp.p.x + palm.centre.x + palm.half.x;
		Check(front <= 1.01 && front > 0.95, "the hand pushed at a wall 30 cm past it stops on the wall's face", front);
		Check(w.HandTouching(h), "and touches it");
		Check(hitMax > 0.3, "the hit on the way in is felt (closing speed)", hitMax);
		// 0.6 m beyond: it is put on the target, as Godot XR Tools' collision hand at its teleport distance
		w.DriveHand(h, Pose{ V3(-0.5, 0, 1), Q() }, dt);
		w.Step(dt);
		w.HandPose(h, hp);
		Check(std::fabs(hp.p.x + 0.5) < 0.02, "a target 1.4 m away: the hand is put there", hp.p.x);
		// free: it follows within a step
		w.DriveHand(h, Pose{ V3(-0.4, 0.1, 1.05), AxisAngle(V3(0, 0, 1), 20) }, dt);
		w.Step(dt);
		w.HandPose(h, hp);
		Check((hp.p - V3(-0.4, 0.1, 1.05)).Length() < 0.005, "free, the hand reaches its target in one step", (hp.p - V3(-0.4, 0.1, 1.05)).Length());
		double y, pi, r;
		ModelAngles(hp.q, y, pi, r);
		Check(std::fabs(y - 20) < 1.0, "and turns with it (yaw 20)", y);
	}
	{
		World w;
		Room(w);
		HandShape palm;
		ItemShape s;
		s.hull = Box(0.05, 0.05, 0.05);
		const Id it = w.AddItem(s, Pose{ V3(0, 0, 0.05), Q() }, V3(), V3());
		for (int i = 0; i < 20; i++) w.Step(dt);
		const Id h = w.AddHand(palm, Pose{ V3(-0.4, 0, 0.05), Q() });
		for (int i = 0; i < 35; i++)
		{
			w.DriveHand(h, Pose{ V3(-0.4 + 0.6 * std::min(1.0, i / 5.0), 0, 0.05), Q() }, dt);   // a swipe: 4 m/s
			w.Step(dt);
		}
		Pose p;
		w.PoseOf(it, p);
		Check(p.p.x > 0.1, "a hand swiped through a box on the floor at 4 m/s knocks it along", p.p.x);
		// a slow reach (1 m/s) into it does not move it: the grip takes it where it is
		Pose before;
		w.PoseOf(it, before);
		w.TeleportHand(h, Pose{ before.p + V3(-0.3, 0, 0), Q() });
		for (int i = 0; i < 12; i++) { w.DriveHand(h, Pose{ before.p + V3(-0.3 + 0.03 * i, 0, 0), Q() }, dt); w.Step(dt); }
		w.PoseOf(it, p);
		Check((p.p - before.p).Length() < 0.005, "a hand reaching into it at 1 m/s leaves it where it lies", (p.p - before.p).Length());
	}
	{
		World w;
		Room(w);
		HandShape palm;
		ItemShape s;
		s.hull = Box(0.15, 0.02, 0.02);   // a 30 cm rod
		const Id h = w.AddHand(palm, Pose{ V3(0.3, 0, 1), Q() });
		w.HandHolds(h, &s, Pose{ V3(0.2, 0, 0), Q() });   // its middle 20 cm ahead of the grip: its tip 35 cm ahead
		for (int i = 0; i < 35; i++) { w.DriveHand(h, Pose{ V3(0.9, 0, 1), Q() }, dt); w.Step(dt); }
		Pose hp;
		w.HandPose(h, hp);
		Check(hp.p.x + 0.35 <= 1.01 && hp.p.x + 0.35 > 0.95, "a rod held 35 cm out stops the hand where its tip meets the wall", hp.p.x + 0.35);
		w.HandHolds(h, nullptr, Pose());
		for (int i = 0; i < 35; i++) { w.DriveHand(h, Pose{ V3(0.9, 0, 1), Q() }, dt); w.Step(dt); }
		w.HandPose(h, hp);
		Check(std::fabs(hp.p.x - 0.9) < 0.01, "let go of it, the hand goes on to its target", hp.p.x);
	}
	{
		// LET GO INSIDE THE PALM: the item leaves the hand it overlaps without being knocked aside
		for (int grace : { 9 })
		{
			World w;
			Room(w);
			HandShape palm;
			const Id h = w.AddHand(palm, Pose{ V3(0, 0, 1.5), Q() });
			ItemShape s;
			s.hull = Box(0.05, 0.05, 0.05);
			// thrown forward at 3 m/s out of a palm moving with it at 3 m/s (fast enough to knock)
			const Id it = w.AddItem(s, Pose{ V3(0.04, 0, 1.5), Q() }, V3(3, 0, 0), V3(), grace ? (int)h : -1, grace);
			for (int i = 0; i < 3; i++) { w.DriveHand(h, Pose{ V3(0.0857 * (i + 1), 0, 1.5), Q() }, dt); w.Step(dt); }
			Pose p;
			w.PoseOf(it, p);
			const double side = std::fabs(p.p.y) + std::fabs(p.p.x - (0.04 + 3.0 * 3 * dt * 0.98));
			Check(side < 0.03, "thrown out of the palm it overlaps, with the hand following through: it flies true (the grace)", side);
		}
	}
	{
		// THE MODEL ANGLES: R = Rz(yaw) Ry(-pitch) Rx(roll) in the map, back to the same three.
		bool all = true;
		double worst = 0;
		for (double yaw : { 0.0, 35.0, -120.0, 179.0 })
			for (double pitch : { 0.0, 20.0, -60.0, 85.0 })
				for (double roll : { 0.0, 45.0, -150.0 })
				{
					const Q q = AxisAngle(V3(0, 0, 1), yaw) * AxisAngle(V3(0, 1, 0), -pitch) * AxisAngle(V3(1, 0, 0), roll);
					double a, b, c;
					ModelAngles(q, a, b, c);
					auto dd = [](double x, double y) { double e = std::fmod(std::fabs(x - y), 360.0); return std::min(e, 360.0 - e); };
					const double e = std::max(dd(a, yaw), std::max(dd(b, pitch), dd(c, roll)));
					worst = std::max(worst, e);
					if (e > 0.01) all = false;
				}
		Check(all, "the model angles come back from the rotation (48 turns, worst error in degrees)", worst);
		const Q q = AxisAngle(V3(1, 2, 3), 77);
		V3 X, Y, Z;
		q.Basis(X, Y, Z);
		const Q r = Q::FromBasis(X, Y, Z);
		Check(std::fabs(std::fabs(q.x * r.x + q.y * r.y + q.z * r.z + q.w * r.w) - 1.0) < 1e-9, "a rotation from its basis is itself");
	}
	printf("physics_test: %d passed, %d failed\n", gPass, gFail);
	return gFail ? 1 : 0;
}
