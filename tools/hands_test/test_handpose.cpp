// tools/hands_test/test_handpose.cpp -- the VR hands' finger math, outside the engine.
//
// Compiles the renderer's own src/r_data/vr_handpose_math.h (nothing copied) and checks it:
//   1. the retention against UZDXREMA's formula at several refresh rates (vr_avatar_pose.cpp:425-431);
//   2. HandFrameFromJoints / DefaultHandFit / ModelSeat on a hand whose answer is known.
// The pose rule, the smoothing and the turn on the joints: test_curls.cpp (the smoothing and the turn differential against UZDXREMA's code).
//
//   g++ -std=c++17 -O1 -Wall -I../../src/r_data test_handpose.cpp -o test_handpose
//   ./test_handpose

#include "vr_handpose_math.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace VRHandPose;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)


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

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	TestRetain();
	TestFrames();
	printf("test_handpose: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
