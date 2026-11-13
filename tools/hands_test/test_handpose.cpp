// tools/hands_test/test_handpose.cpp -- the VR hands' finger math, outside the engine.
//
// Compiles the renderer's own src/r_data/vr_handpose_math.h (nothing copied) and checks it:
//   1. the retention against UZDXREMA's formula at several refresh rates (vr_avatar_pose.cpp:425-431);
//   2. the targets for every named test pose and the edge cases (vr_handpose_math.h Targets);
//   3. the smoothing against UZDXREMA's own two-stage rule replayed here (curl -> QAxisAngle -> gRot slerp),
//      on a fold where the two must agree, and where they are meant to differ (the index on a touch);
//   4. HandFrameFromJoints / DefaultHandFit / ModelSeat on a hand whose answer is known;
//   5. with a pose file from test_iqm.py: each test input run to convergence on the real IQM's frames,
//      the resulting joint rotations written out for test_iqm.py to skin and compare.
//
//   g++ -std=c++17 -O1 -Wall -I../../src/r_data test_handpose.cpp -o test_handpose
//   ./test_handpose [poses.txt out.txt]

#include "vr_handpose_math.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace VRHandPose;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The angle of a^-1 b, as 2 atan2(|v|, |w|): stable near zero, where acos of a dot product near 1
// would read float noise as a twentieth of a degree.
static double QAngleDeg(const float a[4], const float b[4])
{
	const double ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];	// conjugate of a
	const double bx = b[0], by = b[1], bz = b[2], bw = b[3];
	const double w = aw * bw - ax * bx - ay * by - az * bz;
	const double x = aw * bx + ax * bw + ay * bz - az * by;
	const double y = aw * by - ax * bz + ay * bw + az * bx;
	const double z = aw * bz + ax * by - ay * bx + az * bw;
	return 2.0 * atan2(sqrt(x * x + y * y + z * z), fabs(w)) * 57.29577951308232;
}

// ---- UZDXREMA, replayed for comparison: vr_avatar_pose.cpp:410-414 QAxisAngle, :444-448 the curls,
// :666-696 the target and its smoothing. Kept apart from the header so the comparison is not circular.
static void UzQAxisAngle(const float axis[3], float deg, float out[4])
{
	const float h = deg * (3.14159265f / 360.f), s = sinf(h);
	out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s; out[3] = cosf(h);
}
static float UzPerFinger(int f, const Inputs &h)
{
	if (!h.valid) return 0.f;
	switch (f)
	{
	case FINGER_INDEX: return (h.trigger > 0.f) ? h.trigger : (h.triggerTouch ? 0.3f : 0.f);
	case FINGER_THUMB: return h.thumbTouch ? 0.6f : 0.f;
	default: return h.grip;
	}
}
static float UzRetain(double dt)
{
	if (!(dt > 0.0) || dt > 0.25) dt = 1.0 / 90.0;
	return (float)pow(0.75, dt * 90.0);
}

// ---- 1. retention ----
static void TestRetain()
{
	const double rates[] = { 72, 80, 90, 120, 144 };
	for (double hz : rates)
	{
		const double dt = 1.0 / hz;
		CHECK(fabs(Retain(dt) - UzRetain(dt)) < 1e-7, "retain at %.0f Hz: %f vs UZDXREMA %f", hz, Retain(dt), UzRetain(dt));
	}
	CHECK(fabs(Retain(1.0 / 90.0) - 0.75f) < 1e-6, "retain at 90 Hz is %f, not 0.75", Retain(1.0 / 90.0));
	CHECK(Retain(0.0) == UzRetain(0.0) && fabs(Retain(0.0) - 0.75f) < 1e-6, "dt 0 -> 1/90");
	CHECK(fabs(Retain(0.3) - 0.75f) < 1e-6, "dt 0.3 (a hitch) -> 1/90");
	CHECK(fabs(Retain(-1.0) - 0.75f) < 1e-6, "dt < 0 -> 1/90");

	// Frame-rate independence: the fraction left after 100 ms of frames is the same at every rate
	// (to the last frame boundary). 0.75^9 = 0.0751 at 90 Hz.
	for (double hz : rates)
	{
		const int n = (int)lround(0.1 * hz);
		const double left = pow(Retain(1.0 / hz), n);
		const double expect = pow(0.75, 90.0 * n / hz);
		CHECK(fabs(left - expect) < 1e-5, "%.0f Hz: %d frames leave %f, expected %f", hz, n, left, expect);
	}
	// The latency the report quotes: 50% by 27 ms, 90% by 89 ms (continuous time).
	const double t50 = log(0.5) / (90.0 * log(0.75)), t90 = log(0.1) / (90.0 * log(0.75));
	printf("smoothing: 50%% of the way in %.1f ms, 63%% in %.1f ms, 90%% in %.1f ms\n",
		t50 * 1000, log(1 / M_E) / (90.0 * log(0.75)) * 1000, t90 * 1000);
	CHECK(fabs(t50 * 1000 - 26.8) < 0.5 && fabs(t90 * 1000 - 88.9) < 0.5, "latency figures");
}

// ---- 2. targets ----
struct NamedInput { const char *name; float grip, trigger; bool tt, th; int expectFrame; };
// expectFrame: the baked frame every finger should end at (the recipe's own pose), checked on the IQM.
static const NamedInput kNamed[] =
{
	{ "open",      0, 0, false, false, POSE_OPEN },
	{ "fist",      1, 1, true,  true,  6 },	// gripfire: index/MRP fist, thumb 25 down
	{ "grab",      1, 1, true,  true,  6 },
	{ "point",     1, 0, false, true,  POSE_POINT },
	{ "thumbup",   1, 1, true,  false, 10 },	// fire_tu
	{ "fingergun", 1, 0, false, false, POSE_GRIP_TU },
	{ "trigger",   0, 1, true,  false, 2 },	// trigger
	{ "ready",     1, 0, true,  true,  POSE_READY_TD },
};

static void TestTargets()
{
	struct Want { int from[FINGER_COUNT], to[FINGER_COUNT]; float t[FINGER_COUNT]; };
	// [thumb, index, middle, ring, pinky]
	const Want want[] =
	{
		/* open      */ { { 7, 0, 0, 0, 0 }, { 7, 3, 3, 3, 3 }, { 0, 0, 0, 0, 0 } },
		/* fist      */ { { 8, 8, 0, 0, 0 }, { 8, 3, 3, 3, 3 }, { 0, 1, 1, 1, 1 } },
		/* grab      */ { { 8, 8, 0, 0, 0 }, { 8, 3, 3, 3, 3 }, { 0, 1, 1, 1, 1 } },
		/* point     */ { { 1, 0, 0, 0, 0 }, { 1, 3, 3, 3, 3 }, { 0, 0, 1, 1, 1 } },
		/* thumbup   */ { { 7, 8, 0, 0, 0 }, { 7, 3, 3, 3, 3 }, { 0, 1, 1, 1, 1 } },
		/* fingergun */ { { 7, 0, 0, 0, 0 }, { 7, 3, 3, 3, 3 }, { 0, 0, 1, 1, 1 } },
		/* trigger   */ { { 7, 8, 0, 0, 0 }, { 7, 3, 3, 3, 3 }, { 0, 1, 0, 0, 0 } },
		/* ready     */ { { 8, 8, 0, 0, 0 }, { 8, 3, 3, 3, 3 }, { 0, 0, 1, 1, 1 } },
	};
	for (size_t k = 0; k < sizeof(kNamed) / sizeof(kNamed[0]); k++)
	{
		Inputs in; in.grip = kNamed[k].grip; in.trigger = kNamed[k].trigger;
		in.triggerTouch = kNamed[k].tt; in.thumbTouch = kNamed[k].th; in.valid = true;
		FingerTarget t[FINGER_COUNT];
		Targets(in, t);
		for (int f = 0; f < FINGER_COUNT; f++)
		{
			// A finger whose t is 0 is at its `from` whatever `to` says; compare what is drawn.
			const bool same = t[f].from == want[k].from[f] && fabs(t[f].t - want[k].t[f]) < 1e-6
				&& (want[k].t[f] == 0.f || t[f].to == want[k].to[f]);
			CHECK(same, "%s finger %d: from %d to %d t %.2f, want from %d to %d t %.2f", kNamed[k].name, f,
				t[f].from, t[f].to, t[f].t, want[k].from[f], want[k].to[f], want[k].t[f]);
		}
	}
	// Edge cases.
	Inputs in; in.valid = false; in.grip = 1; in.trigger = 1; in.thumbTouch = in.triggerTouch = true;
	FingerTarget t[FINGER_COUNT];
	Targets(in, t);
	bool allOpen = true;
	for (int f = 0; f < FINGER_COUNT; f++) allOpen &= (t[f].from == POSE_OPEN && t[f].t == 0.f);
	CHECK(allOpen, "no controller -> every finger open (UZDXREMA :450-454)");
	in = Inputs(); in.valid = true; in.grip = 1.7f; in.trigger = -0.2f;
	Targets(in, t);
	CHECK(t[FINGER_MIDDLE].t == 1.f && t[FINGER_INDEX].t == 0.f, "inputs clamped to 0..1");
	in = Inputs(); in.valid = true; in.trigger = 0.4f;	// a controller with no capacitive trigger
	Targets(in, t);
	CHECK(t[FINGER_INDEX].from == POSE_OPEN && t[FINGER_INDEX].to == POSE_FIST && fabs(t[FINGER_INDEX].t - 0.4f) < 1e-6,
		"no trigger touch: open -> fist by the trigger, UZDXREMA's curve");
	in.thumbTouch = true;
	Targets(in, t);
	CHECK(t[FINGER_THUMB].from == POSE_READY_TD, "trigger pulled without a touch still counts as the index on the trigger");
}

// ---- 3. smoothing, against UZDXREMA's own two stages ----
// A synthetic pose table: every joint's open is identity and its fist a fold about a fixed axis
// (FRIK's middle-finger numbers, vrhandcurl.txt), so slerp(open, fist, c) and UZDXREMA's
// QAxisAngle(axis, deg * c) are the same rotation. The smoothed traces must then agree to float
// rounding; with the index on a touch they must NOT (the departure), and the size of it is printed.
static void TestSmoothingAgainstUZ()
{
	const float axis[FINGER_COUNT][FINGER_JOINTS][3] =
	{
		{ { -0.42934f, 0.55813f, 0.71004f }, { 0.13967f, -0.30794f, 0.94110f }, { 0, 0, 1 } },
		{ { -0.11502f, 0.00649f, 0.99334f }, { 0, 0, 1 }, { 0, 0, 1 } },
		{ { -0.07335f, -0.00858f, 0.99727f }, { 0, 0, 1 }, { 0, 0, 1 } },
		{ { -0.10693f, 0.03578f, 0.99362f }, { 0, 0, 1 }, { 0, 0, 1 } },
		{ { -0.13472f, -0.07973f, 0.98767f }, { 0, 0, 1 }, { 0, 0, 1 } },
	};
	const float deg[FINGER_COUNT][FINGER_JOINTS] =
	{
		{ 22.171f, 20.824f, 64.031f }, { 73.379f, 101.663f, 89.983f }, { 63.789f, 91.660f, 76.790f },
		{ 61.510f, 89.488f, 61.137f }, { 59.653f, 80.361f, 67.323f },
	};
	PoseTable poses;
	for (int p = 0; p <= POSE_MAX_USED; p++)
		for (int f = 0; f < FINGER_COUNT; f++)
			for (int j = 0; j < FINGER_JOINTS; j++)
			{
				float a[3]; float n = 0;
				for (int k = 0; k < 3; k++) n += axis[f][j][k] * axis[f][j][k];
				n = sqrtf(n);
				for (int k = 0; k < 3; k++) a[k] = axis[f][j][k] / n;
				// open = identity; every other pose here = the full fold, except ready_td's index = 0.3 of it
				// (so the index-on-touch case is UZDXREMA's 0.3 exactly and only the pull differs).
				float c = (p == POSE_OPEN) ? 0.f : 1.f;
				if (p == POSE_READY_TD && f == FINGER_INDEX) c = 0.3f;
				if (f == FINGER_THUMB) c = (p == POSE_OPEN || p == POSE_GRIP_TU) ? 0.f : 0.6f;
				UzQAxisAngle(a, deg[f][j] * c, poses.Q[p][f][j]);
			}

	// An input script: 2 s at a given rate, grip and trigger ramping, touches toggling.
	auto input = [](double t) {
		Inputs in; in.valid = true;
		in.grip = (float)(t < 0.3 ? 0 : t < 0.6 ? (t - 0.3) / 0.3 : t < 1.2 ? 1 : 0.2);
		in.thumbTouch = (t > 0.5 && t < 1.5);
		in.triggerTouch = (t > 0.8);
		in.trigger = (float)(t < 1.0 ? 0 : t < 1.3 ? (t - 1.0) / 0.3 : 1);
		if (t > 1.7) in.valid = false;
		return in;
	};
	const double rates[] = { 72, 90, 120 };
	for (double hz : rates)
	{
		SideState ours;
		float uz[FINGER_COUNT][FINGER_JOINTS][4]; bool uzInit = false;
		double worstSame = 0, worstIndex = 0, worstIndexTouchOnly = 0;
		const int n = (int)(2.0 * hz);
		for (int i = 0; i < n; i++)
		{
			const double t = i / hz, dt = (i == 0) ? 0.0 : 1.0 / hz;
			const Inputs in = input(t);
			Step(ours, in, poses, dt);
			const float retain = UzRetain(dt);
			for (int f = 0; f < FINGER_COUNT; f++)
				for (int j = 0; j < FINGER_JOINTS; j++)
				{
					float a[3]; float nn = 0;
					for (int k = 0; k < 3; k++) nn += axis[f][j][k] * axis[f][j][k];
					nn = sqrtf(nn);
					for (int k = 0; k < 3; k++) a[k] = axis[f][j][k] / nn;
					float wq[4];
					UzQAxisAngle(a, deg[f][j] * UzPerFinger(f, in), wq);
					float *cur = uz[f][j];
					if (!uzInit) for (int k = 0; k < 4; k++) cur[k] = wq[k];
					else { float o[4]; QSlerp(cur, wq, 1.f - retain, o); for (int k = 0; k < 4; k++) cur[k] = o[k]; }
					const double d = QAngleDeg(cur, ours.Q[f][j]);
					if (f == FINGER_INDEX)
					{
						if (d > worstIndex) worstIndex = d;
						if (in.triggerTouch && in.trigger == 0.f && d > worstIndexTouchOnly) worstIndexTouchOnly = d;
					}
					else if (d > worstSame) worstSame = d;
				}
			uzInit = true;
		}
		printf("smoothing vs UZDXREMA at %3.0f Hz: thumb/middle/ring/pinky worst %.5f deg; index worst %.2f deg "
			"(the touch-then-pull departure), index on a touch before any pull %.5f deg\n",
			hz, worstSame, worstIndex, worstIndexTouchOnly);
		CHECK(worstSame < 0.01, "%.0f Hz: thumb and grip fingers differ from UZDXREMA by %.4f deg", hz, worstSame);
		CHECK(worstIndexTouchOnly < 0.01, "%.0f Hz: index on a touch differs from UZDXREMA's 0.3 by %.4f deg", hz, worstIndexTouchOnly);
		CHECK(worstIndex > 1.0, "%.0f Hz: the touch-then-pull departure should show (got %.4f deg)", hz, worstIndex);
	}
}

// ---- 4. the hand frame, the fit and the seat ----
static void TestFrames()
{
	// A right hand in a right-handed space: fingers +X, thumb side +Y, back of the hand +Z (palm -Z).
	const Vec3d P[5] = { { 0, 0, 0 }, { 11, 2, 1 }, { 11, 0.2, 1.5 }, { 10.8, -1.6, 1.4 }, { 10.5, -3.1, 1.1 } };
	Vec3d X, Y, Z, s;
	CHECK(HandFrameFromJoints(P, true, X, Y, Z, s), "right hand frame");
	CHECK(X.x > 0.98, "X along the fingers (%f %f %f)", X.x, X.y, X.z);
	CHECK(Y.z > 0.9, "Y = back of the hand = +Z here (%f %f %f)", Y.x, Y.y, Y.z);
	CHECK(Z.y < -0.9, "Z = X x Y: the right thumb (+Y) on -Z (%f %f %f)", Z.x, Z.y, Z.z);
	const double det = X.Dot(Y.Cross(Z));
	CHECK(fabs(det - 1) < 1e-9, "proper rotation, det %f", det);
	// The socket below the palm (-Y) and toward the wrist (-X) from the knuckle centroid.
	const Vec3d cen = (P[1] + P[2] + P[3] + P[4]) * 0.25;
	CHECK((s - cen).Dot(Y) < 0 && (s - cen).Dot(X) < 0, "socket in front of the palm, behind the knuckles");

	// The mirror image is a left hand and gets the mirrored frame (thumb on +Z).
	Vec3d Pl[5];
	for (int i = 0; i < 5; i++) Pl[i] = Vec3d(P[i].x, -P[i].y, P[i].z);
	Vec3d Xl, Yl, Zl, sl;
	CHECK(HandFrameFromJoints(Pl, false, Xl, Yl, Zl, sl), "left hand frame");
	CHECK(Yl.z > 0.9 && Zl.y < -0.9, "left: back +Z, Z = X x Y = -Y, so the left thumb (-Y) is on +Z");
	CHECK(fabs(sl.y + s.y) < 1e-9 && fabs(sl.x - s.x) < 1e-9, "left socket mirrors the right");

	// DefaultHandFit: proper rotations, the table in UZDXREMA's comment.
	for (int r = 0; r < 2; r++)
	{
		float m[16];
		DefaultHandFit(r == 0, m);
		const double d = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
		CHECK(fabs(d - 1) < 1e-6, "DefaultHandFit(%s) det %f", r == 0 ? "right" : "left", d);
		CHECK(m[1] == -1.f && m[4] == (r == 0 ? 1.f : -1.f) && m[10] == (r == 0 ? 1.f : -1.f), "DefaultHandFit table");
	}

	// ModelSeat: render coords (x, z, y) of the file; the socket lands at the origin, the knuckles on +X,
	// and the seat is a reflection (det -1) -- the one that undoes the loader's swap.
	float seat[16];
	Vec3d sock;
	CHECK(ModelSeat(P, true, seat, &sock), "seat");
	auto apply = [&](const Vec3d &file) {
		const double r[3] = { file.x, file.z, file.y };	// file -> render (the loader's swap)
		Vec3d o;
		o.x = seat[0] * r[0] + seat[4] * r[1] + seat[8] * r[2] + seat[12];
		o.y = seat[1] * r[0] + seat[5] * r[1] + seat[9] * r[2] + seat[13];
		o.z = seat[2] * r[0] + seat[6] * r[1] + seat[10] * r[2] + seat[14];
		return o;
	};
	const Vec3d so = apply(sock);
	CHECK(so.Length() < 1e-5, "socket -> origin (%f %f %f)", so.x, so.y, so.z);
	const Vec3d mo = apply(P[2]) - apply(P[0]);
	CHECK(mo.x > 10.9 && fabs(mo.y) < 1e-5 && fabs(mo.z) < 1e-5, "wrist->middle knuckle along hand X (%f %f %f)", mo.x, mo.y, mo.z);
	const double sd = seat[0] * (seat[5] * seat[10] - seat[9] * seat[6]) - seat[4] * (seat[1] * seat[10] - seat[9] * seat[2]) + seat[8] * (seat[1] * seat[6] - seat[5] * seat[2]);
	CHECK(fabs(sd + 1) < 1e-5, "seat det %f (want -1)", sd);
}

// ---- 5. the real IQM's poses, from test_iqm.py ----
// poses.txt: lines "q <pose> <finger> <joint> x y z w". out.txt: "r <name> <finger> <joint> x y z w" after
// 2 s at 90 Hz of each named input, plus a few analog ones.
static int RunPoseFile(const char *in, const char *out)
{
	FILE *fi = fopen(in, "r");
	if (!fi) { printf("no %s\n", in); return 1; }
	PoseTable poses;
	Vec3d P[5];
	int n = 0, np = 0;
	char line[512];
	while (fgets(line, sizeof line, fi))
	{
		int p, f, j; float q[4]; double x, y, z;
		if (sscanf(line, "q %d %d %d %f %f %f %f", &p, &f, &j, &q[0], &q[1], &q[2], &q[3]) == 7)
		{
			if (p < 0 || p > POSE_MAX_USED || f < 0 || f >= FINGER_COUNT || j < 0 || j >= FINGER_JOINTS) continue;
			memcpy(poses.Q[p][f][j], q, sizeof q);
			n++;
		}
		else if (sscanf(line, "p %d %lf %lf %lf", &p, &x, &y, &z) == 4 && p >= 0 && p < 5)
		{
			P[p] = Vec3d(x, y, z);
			np++;
		}
	}
	fclose(fi);
	printf("pose file: %d joint rotations, %d frame points\n", n, np);
	FILE *fo = fopen(out, "w");
	// The seat the renderer will use for this model, both sides (vr_handpose_math.h ModelSeat).
	for (int side = 0; side < SIDE_COUNT && np == 5; side++)
	{
		float m[16];
		Vec3d sock;
		if (!ModelSeat(P, side == SIDE_RIGHT, m, &sock)) { fprintf(fo, "seat %d none\n", side); continue; }
		fprintf(fo, "seat %d", side);
		for (float v : m) fprintf(fo, " %.9g", v);
		fprintf(fo, "\nsocket %d %.6f %.6f %.6f\n", side, sock.x, sock.y, sock.z);
		float fit[16];
		DefaultHandFit(side == SIDE_RIGHT, fit);
		fprintf(fo, "fit %d", side);
		for (float v : fit) fprintf(fo, " %.9g", v);
		fprintf(fo, "\n");
	}
	struct Run { std::string name; Inputs in; };
	std::vector<Run> runs;
	for (const NamedInput &k : kNamed)
	{
		Run r; r.name = k.name; r.in.grip = k.grip; r.in.trigger = k.trigger; r.in.triggerTouch = k.tt; r.in.thumbTouch = k.th; r.in.valid = true;
		runs.push_back(r);
	}
	const float halves[] = { 0.25f, 0.5f, 0.75f };
	for (float h : halves)
	{
		Run r; r.in.valid = true; r.in.grip = h; r.in.trigger = h; r.in.triggerTouch = true; r.in.thumbTouch = true;
		char nm[32]; snprintf(nm, sizeof nm, "analog%.2f", h); r.name = nm;
		runs.push_back(r);
	}
	for (const Run &r : runs)
	{
		SideState s;
		// From a different pose first, so convergence is tested, not just the first-frame copy.
		Inputs start; start.valid = true; start.grip = 1.f - r.in.grip; start.thumbTouch = !r.in.thumbTouch;
		for (int i = 0; i < 30; i++) Step(s, start, poses, 1.0 / 90.0);
		for (int i = 0; i < 180; i++) Step(s, r.in, poses, 1.0 / 90.0);
		for (int ff = 0; ff < FINGER_COUNT; ff++)
			for (int jj = 0; jj < FINGER_JOINTS; jj++)
				fprintf(fo, "r %s %d %d %.9f %.9f %.9f %.9f\n", r.name.c_str(), ff, jj,
					s.Q[ff][jj][0], s.Q[ff][jj][1], s.Q[ff][jj][2], s.Q[ff][jj][3]);
		// and the expected frame for the named ones
		for (const NamedInput &k : kNamed)
			if (r.name == k.name) fprintf(fo, "e %s %d\n", k.name, k.expectFrame);
	}
	// A step response on the real poses: open -> fist grip at 90 Hz, middle finger's base joint, per frame.
	{
		SideState s;
		Inputs a; a.valid = true;
		Step(s, a, poses, 0.0);
		Inputs b = a; b.grip = 1.f;
		const double full = QAngleDeg(poses.Q[POSE_OPEN][FINGER_MIDDLE][1], poses.Q[POSE_FIST][FINGER_MIDDLE][1]);
		for (int i = 1; i <= 12; i++)
		{
			Step(s, b, poses, 1.0 / 90.0);
			const double got = QAngleDeg(poses.Q[POSE_OPEN][FINGER_MIDDLE][1], s.Q[FINGER_MIDDLE][1]);
			fprintf(fo, "step %d %.4f %.4f\n", i, got / full, 1.0 - pow(0.75, i));
		}
	}
	fclose(fo);
	return 0;
}

int main(int argc, char **argv)
{
	TestRetain();
	TestTargets();
	TestSmoothingAgainstUZ();
	TestFrames();
	if (argc >= 3 && RunPoseFile(argv[1], argv[2]) != 0) g_fail++;
	printf("test_handpose: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
