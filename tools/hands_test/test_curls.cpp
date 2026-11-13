// tools/hands_test/test_curls.cpp -- DIFFERENTIAL TEST of the VR hands' smoothing and the turn on the joints against
// UZDXREMA's OWN CODE, and the pose rule's cases.
//
// run.sh cuts these lines out of UZDXREMA (read-only, HEAD 39e4dad) and they are #included below, unedited:
//   vr_avatar_pose.cpp:336-348   FromMat3Pos
//   vr_avatar_pose.cpp:392-414   QSlerp, QAxisAngle
//   vr_avatar_pose.cpp:429-431   the rate (dt sanitised, retain = 0.75^(dt*90))
//   vr_avatar_pose.cpp:691-693   gRot's smoothing of one joint toward its target wq: first frame the target, then
//                                slerp(cur, wq, 1 - retain)
//   vr_avatar_pose.cpp:696       gRot[sd].Init = true
//   vr_avatar_pose.cpp:1785-1836 the turn D = F.R(q).F^T on the joint globals and the carry of the finger
//   vr_armik.h:35-54             FVRMat3
// The only things supplied here are the engine names those lines use (VSMatrix with multMatrix/inverseMatrix,
// FVector3, the side state).
//
// THE RULE IS NOT UZDXREMA'S ANY MORE (owner, 2026-11-13: "the hands have poses, why are we bending fingers"): the
// touches pick an authored pose and the grip / trigger say how far toward it (vr_handpose_math.h PoseRule). Its cases
// are checked here by value; the poses themselves on the real FBX in tools/fbxload_test. What is still the source's
// is the rate and the smoothing: the port's StepPose and the source's own lines are fed the same targets, frame by
// frame, at 72, 80, 90, 120 and 144 Hz with jittered and hitched frame times, poses switching, controllers dropping
// out; every smoothed quaternion (the 15 finger joints and the palm) is compared every frame. Then the turn on random
// skeletons.
//
//   ./test_curls

#include "vr_handpose_math.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <array>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ======================================================================= the source, as cut
namespace UZ
{
	// --- the engine names the cut lines use
	struct FVector3
	{
		float X = 0, Y = 0, Z = 0;
		FVector3() = default;
		FVector3(float x, float y, float z) : X(x), Y(y), Z(z) {}
	};
	inline float operator|(const FVector3 &a, const FVector3 &b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }

	struct VSMatrix
	{
		float m[16];
		void loadMatrix(const float *v) { memcpy(m, v, sizeof(m)); }
		const float *get() const { return m; }
		void multMatrix(const VSMatrix &b) { VRHandPose::Detail::Mul4(m, b.m, m); }
		bool inverseMatrix(VSMatrix &out) { return VRHandPose::Detail::Inverse4(m, out.m); }
	};

	enum { VRFINGER_THUMB = 0, VRFINGER_INDEX, VRFINGER_MIDDLE, VRFINGER_RING, VRFINGER_PINKY, VRFINGER_COUNT };
	enum { VRFINGER_JOINTS = 3 };
	enum { VRRIG_MAIN = 0, VRRIG_OFF = 1 };

#include "fvrmat3.inc"
#include "frommat3pos.inc"
#include "qmath.inc"

	struct FRotState { float Q[VRFINGER_COUNT][VRFINGER_JOINTS][4] = {}; bool Init = false; };
	struct FVRAvatarFingerJoint { int Joint = -1; FVector3 Frame[3] = { FVector3(1, 0, 0), FVector3(0, 1, 0), FVector3(0, 0, 1) }; };
	struct FVRAvatarSide { FVRAvatarFingerJoint Fingers[VRFINGER_COUNT][VRFINGER_JOINTS]; };

	FRotState gRot[2];

	float Retain(double dt)
	{
#include "retain.inc"
		return retain;
	}

	// gRot's smoothing of joint (f, j) of side sd toward wq (VAP :691-693)
	void SmoothJoint(int sd, int f, int j, const float wq[4], float retain)
	{
#include "gslerp.inc"
	}
	void EndSide(int sd)
	{
#include "ginit.inc"
	}

	void Turn(int sd, const FVRAvatarSide &side, std::vector<VSMatrix> &gGlobals)
	{
		const int n = (int)gGlobals.size();
#include "turn.inc"
	}
}

// ======================================================================= 1. the rule and the smoothing
static double QDiff(const float a[4], const float b[4])
{
	double d = 0;
	for (int k = 0; k < 4; k++) d = fmax(d, fabs((double)a[k] - b[k]));
	return d;
}

static void RandQ(std::mt19937 &rng, float q[4])
{
	std::normal_distribution<float> g(0.f, 1.f);
	float n = 0;
	for (int k = 0; k < 4; k++) { q[k] = g(rng); n += q[k] * q[k]; }
	n = sqrtf(n);
	for (int k = 0; k < 4; k++) q[k] /= n;
}

// The smoothing against the source's own lines. The port's StepPose makes the targets (PoseRule, PoseTargets: not the
// source's) and smooths them; the source's lines smooth the SAME targets. Side 0 carries the 15 finger joints, side 1
// the palm (as joint 0/0: the port smooths the palm with the same formula).
static void TestSmoothing()
{
	using namespace VRHandPose;
	std::mt19937 rng(20261113);
	std::uniform_real_distribution<float> u01(0.f, 1.f);
	HandPoses poses;
	for (int f = 0; f < 5; f++)
		for (int j = 0; j < 3; j++)
		{
			RandQ(rng, poses.Open[f][j]);
			for (int p = 0; p < POSE_COUNT; p++) RandQ(rng, poses.Target[p][f][j]);
		}
	RandQ(rng, poses.PalmOpen);
	for (int p = 0; p < POSE_COUNT; p++) { RandQ(rng, poses.PalmTarget[p]); poses.PalmDriven[p] = (p == POSE_FIST); }
	poses.Have = true;

	const double rates[] = { 72, 80, 90, 120, 144 };
	for (double hz : rates)
	{
		for (int s = 0; s < 2; s++) UZ::gRot[s] = UZ::FRotState();
		SideState pr; PalmState pp;
		double worstQ = 0, worstPalm = 0, worstRetain = 0;
		int frames = 0, switches = 0, lastPose = -1;
		for (int fr = 0; fr < 3000; fr++)
		{
			// frame time: the rate, jittered 10%; every 400th a hitch (0.3 s), every 777th a repeat (0)
			double dt = (1.0 / hz) * (0.9 + 0.2 * u01(rng));
			if (fr % 400 == 399) dt = 0.3;
			if (fr % 777 == 776) dt = 0.0;
			const float rp = Retain(dt), ru = UZ::Retain(dt);
			worstRetain = fmax(worstRetain, fabs((double)rp - ru));

			// inputs that hold for a while and then jump (presses, releases, touches), and drift
			Inputs in;
			const int phase = (fr / 37) % 11;
			in.trigger = (phase % 3 == 0) ? 0.f : (phase % 3 == 1 ? 1.f : u01(rng));
			in.grip = (phase % 4 == 0) ? 0.f : (phase % 4 == 1 ? 1.f : u01(rng));
			in.triggerTouch = (phase % 2) == 0 || in.trigger > 0.f;
			in.thumbTouch = (phase % 5) < 2;
			in.valid = !((fr / 151) % 7 == 3);	// a dropout now and then

			StepPose(pr, pp, in, rp, &poses);

			PoseInputs pi;
			PoseRule(in, pi);
			if (pi.Pose != lastPose) { switches++; lastPose = pi.Pose; }
			float target[5][3][4], palm[4];
			PoseTargets(&poses, pi, target, palm);
			for (int f = 0; f < 5; f++)
				for (int j = 0; j < 3; j++)
				{
					UZ::SmoothJoint(0, f, j, target[f][j], ru);
					worstQ = fmax(worstQ, QDiff(pr.Q[f][j], UZ::gRot[0].Q[f][j]));
				}
			UZ::EndSide(0);
			UZ::SmoothJoint(1, 0, 0, palm, ru);
			UZ::EndSide(1);
			worstPalm = fmax(worstPalm, QDiff(pp.Q, UZ::gRot[1].Q[0][0]));
			frames++;
		}
		printf("smoothing, %3.0f Hz: %d frames, %d pose switches, 15 joints + the palm: max |q - source| %.3g (palm %.3g), max |retain - source| %.3g\n",
			hz, frames, switches, worstQ, worstPalm, worstRetain);
		CHECK(worstQ == 0.0 && worstPalm == 0.0 && worstRetain == 0.0, "smoothing differs from UZDXREMA's own lines at %.0f Hz (q %.3g, palm %.3g, retain %.3g)",
			hz, worstQ, worstPalm, worstRetain);
	}
}

// The rule's cases, said out loud.
static void TestRule()
{
	using namespace VRHandPose;
	struct Case { const char *name; float grip, trigger; bool tt, th, valid; int pose; float want[5]; float palm; };
	const Case cases[] =
	{
		{ "open",            0,   0,   false, false, true,  POSE_FINGERGUN, { 0.f, 0.f, 0.f, 0.f, 0.f }, 0.f },
		{ "fist",            1,   1,   true,  true,  true,  POSE_FIST,      { 1.f, 1.f, 1.f, 1.f, 1.f }, 1.f },
		{ "point",           1,   0,   false, true,  true,  POSE_POINT,     { 1.f, 1.f, 1.f, 1.f, 1.f }, 1.f },
		{ "thumb up",        1,   1,   true,  false, true,  POSE_THUMBUP,   { 1.f, 1.f, 1.f, 1.f, 1.f }, 1.f },
		{ "finger gun",      1,   0,   false, false, true,  POSE_FINGERGUN, { 1.f, 1.f, 1.f, 1.f, 1.f }, 1.f },
		{ "trigger only",    0,   1,   true,  false, true,  POSE_THUMBUP,   { 0.f, 1.f, 0.f, 0.f, 0.f }, 0.f },
		{ "trigger + thumb", 0,   1,   true,  true,  true,  POSE_FIST,      { 0.f, 1.f, 0.f, 0.f, 0.f }, 0.f },
		{ "trigger touch",   0,   0,   true,  false, true,  POSE_THUMBUP,   { 0.f, 0.3f, 0.f, 0.f, 0.f }, 0.f },
		{ "grab, resting",   1,   0,   true,  true,  true,  POSE_FIST,      { 1.f, 0.3f, 1.f, 1.f, 1.f }, 1.f },
		{ "half grip point", 0.5f, 0,  false, true,  true,  POSE_POINT,     { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f }, 0.5f },
		{ "half pull",       1,   0.5f, true, true,  true,  POSE_FIST,      { 1.f, 0.5f, 1.f, 1.f, 1.f }, 1.f },
		{ "no controller",   1,   1,   true,  true,  false, POSE_FIST,      { 0.f, 0.f, 0.f, 0.f, 0.f }, 0.f },
	};
	for (const Case &c : cases)
	{
		Inputs in; in.grip = c.grip; in.trigger = c.trigger; in.triggerTouch = c.tt; in.thumbTouch = c.th; in.valid = c.valid;
		PoseInputs pi;
		PoseRule(in, pi);
		bool ok = pi.Pose == c.pose && pi.Palm == c.palm;
		for (int f = 0; f < 5; f++) ok = ok && pi.Amount[f] == c.want[f];
		static const char *const names[POSE_COUNT] = { "fist", "point", "thumbup", "fingergun" };
		printf("  %-16s pose %-9s thumb %.1f index %.1f middle %.1f ring %.1f pinky %.1f palm %.1f\n", c.name, names[pi.Pose],
			pi.Amount[0], pi.Amount[1], pi.Amount[2], pi.Amount[3], pi.Amount[4], pi.Palm);
		CHECK(ok, "rule: %s", c.name);
	}
}

// The ends of a target: amount 0 is the open pose, amount 1 the authored pose; a pose that does not drive the palm
// leaves it at its open rotation; no poses -> identity (the drawn frame).
static void TestPoseTarget()
{
	using namespace VRHandPose;
	std::mt19937 rng(7);
	HandPoses poses;
	for (int f = 0; f < 5; f++)
		for (int j = 0; j < 3; j++)
		{
			RandQ(rng, poses.Open[f][j]);
			for (int p = 0; p < POSE_COUNT; p++) RandQ(rng, poses.Target[p][f][j]);
		}
	RandQ(rng, poses.PalmOpen);
	for (int p = 0; p < POSE_COUNT; p++) { RandQ(rng, poses.PalmTarget[p]); poses.PalmDriven[p] = (p == POSE_FIST); }
	poses.Have = true;
	auto same = [](const float *a, const float *b) { return fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) > 1 - 1e-6; };
	for (int p = 0; p < POSE_COUNT; p++)
	{
		PoseInputs pi; pi.Pose = p;
		float t[5][3][4], palm[4];
		for (int f = 0; f < 5; f++) pi.Amount[f] = 0.f;
		pi.Palm = 0.f;
		PoseTargets(&poses, pi, t, palm);
		bool ok0 = same(palm, poses.PalmOpen), ok1 = true;
		for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) ok0 = ok0 && same(t[f][j], poses.Open[f][j]);
		for (int f = 0; f < 5; f++) pi.Amount[f] = 1.f;
		pi.Palm = 1.f;
		PoseTargets(&poses, pi, t, palm);
		for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) ok1 = ok1 && same(t[f][j], poses.Target[p][f][j]);
		ok1 = ok1 && same(palm, poses.PalmDriven[p] ? poses.PalmTarget[p] : poses.PalmOpen);
		CHECK(ok0, "pose %d at amount 0 is the open pose", p);
		CHECK(ok1, "pose %d at amount 1 is the authored pose (the palm: its pose's frame only where driven)", p);
	}
	PoseInputs pi;
	float t[5][3][4], palm[4];
	PoseTargets(nullptr, pi, t, palm);
	CHECK(t[1][0][3] == 1.f && palm[3] == 1.f, "no poses: identity");
}

// ======================================================================= 3. the turn on the joints
static void RandRot(std::mt19937 &rng, float R[3][3])	// columns
{
	std::normal_distribution<float> g(0.f, 1.f);
	float q[4] = { g(rng), g(rng), g(rng), g(rng) };
	const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (float &v : q) v /= n;
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	const float c[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) },
							{ 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) },
							{ 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) } };
	memcpy(R, c, sizeof(c));
}

static void TestTurn()
{
	using namespace VRHandPose;
	std::mt19937 rng(99);
	std::normal_distribution<float> g(0.f, 1.f);
	double worst = 0, worstTip = 0;
	for (int trial = 0; trial < 200; trial++)
	{
		// a hand: joint 0 the wrist (mirrored on odd trials, scaled), then per finger 4 joints in a chain (BASE MID TOP UP_TOP)
		const int n = 1 + 5 * 4;
		std::vector<int> parent(n, -1);
		std::vector<std::array<float, 16>> local(n);
		for (int i = 0; i < n; i++)
		{
			float R[3][3];
			RandRot(rng, R);
			const float s = (i == 0) ? (0.5f + (trial % 5) * 0.3f) : 1.f;
			const float mx = (i == 0 && (trial & 1)) ? -1.f : 1.f;
			float *m = local[i].data();
			memset(m, 0, 64);
			for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) m[c * 4 + r] = R[c][r] * s * (c == 0 ? mx : 1.f);
			m[12] = g(rng); m[13] = g(rng); m[14] = g(rng); m[15] = 1;
			if (i > 0) parent[i] = ((i - 1) % 4 == 0) ? 0 : i - 1;
		}
		std::vector<std::array<float, 16>> glob(n);
		for (int i = 0; i < n; i++)
		{
			if (parent[i] < 0) glob[i] = local[i];
			else Detail::Mul4(glob[parent[i]].data(), local[i].data(), glob[i].data());
		}
		HandRig rig;
		UZ::FVRAvatarSide side;
		SideState rot;
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++)
			{
				const int jt = 1 + f * 4 + j;
				rig.Joint[f][j] = jt;
				side.Fingers[f][j].Joint = jt;
				float R[3][3];
				RandRot(rng, R);
				for (int c = 0; c < 3; c++)
				{
					for (int k = 0; k < 3; k++) rig.Frame[f][j][c][k] = R[c][k];
					side.Fingers[f][j].Frame[c] = UZ::FVector3(R[c][0], R[c][1], R[c][2]);
				}
				// the source's set: the finger's own later joints
				for (int k = j + 1; k < 3; k++) rig.Below[f][j].push_back(1 + f * 4 + k);
				float q[4] = { g(rng), g(rng), g(rng), g(rng) };
				const float nq = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
				for (int k = 0; k < 4; k++) rot.Q[f][j][k] = q[k] / nq;
				UZ::gRot[0].Q[f][j][0] = rot.Q[f][j][0]; UZ::gRot[0].Q[f][j][1] = rot.Q[f][j][1];
				UZ::gRot[0].Q[f][j][2] = rot.Q[f][j][2]; UZ::gRot[0].Q[f][j][3] = rot.Q[f][j][3];
			}
		// one finger joint left unturned, as the source skips (|w| > 0.999999)
		rot.Q[2][1][0] = rot.Q[2][1][1] = rot.Q[2][1][2] = 0; rot.Q[2][1][3] = 1;
		for (int k = 0; k < 4; k++) UZ::gRot[0].Q[2][1][k] = rot.Q[2][1][k];

		std::vector<UZ::VSMatrix> ug(n);
		for (int i = 0; i < n; i++) ug[i].loadMatrix(glob[i].data());
		UZ::Turn(0, side, ug);

		std::vector<std::array<float, 16>> pg = glob;
		std::vector<char> moved;
		ApplyFingerTurns(reinterpret_cast<float (*)[16]>(pg.data()), n, rig, rot, moved);
		for (int i = 0; i < n; i++)
			for (int k = 0; k < 16; k++) worst = fmax(worst, fabs((double)pg[i][k] - ug[i].m[k]));

		// with the tip (UP_TOP) in Below as the engine builds it: joints 1-3 unchanged; the tip carried rigidly
		HandRig rig2 = rig;
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++) rig2.Below[f][j].push_back(1 + f * 4 + 3);
		std::vector<std::array<float, 16>> pg2 = glob;
		ApplyFingerTurns(reinterpret_cast<float (*)[16]>(pg2.data()), n, rig2, rot, moved);
		for (int f = 0; f < 5; f++)
		{
			for (int j = 0; j < 3; j++)
				for (int k = 0; k < 16; k++) worst = fmax(worst, fabs((double)pg2[1 + f * 4 + j][k] - ug[1 + f * 4 + j].m[k]));
			// the tip keeps its rest LOCAL transform under the posed TOP joint
			float inv0[16], l0[16], inv1[16], l1[16];
			const int top = 1 + f * 4 + 2, tip = top + 1;
			Detail::Inverse4(glob[top].data(), inv0); Detail::Mul4(inv0, glob[tip].data(), l0);
			Detail::Inverse4(pg2[top].data(), inv1); Detail::Mul4(inv1, pg2[tip].data(), l1);
			for (int k = 0; k < 16; k++) worstTip = fmax(worstTip, fabs((double)l0[k] - l1[k]));
		}
	}
	printf("turn on the joints: 200 random hands (mirrored and scaled wrists), 15 joints each: max |matrix - source| %.3g; "
		"with the tip carried: joints 1-3 identical, the tip's local under TOP kept to %.3g\n", worst, worstTip);
	CHECK(worst < 1e-5, "turn differs from the source (%.3g)", worst);
	CHECK(worstTip < 1e-4, "tip not carried rigidly (%.3g)", worstTip);
}

// ======================================================================= 4. palette <-> global round trip
static void TestPalette()
{
	using namespace VRHandPose;
	std::mt19937 rng(5);
	std::normal_distribution<float> g(0.f, 1.f);
	double worst = 0;
	for (int t = 0; t < 100; t++)
	{
		float R[3][3], base[16] = { 0 }, pal[16] = { 0 }, inv[16], G[16], back[16];
		RandRot(rng, R);
		for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) base[c * 4 + r] = R[c][r] * (c == 0 && (t & 1) ? -1.f : 1.f);
		base[12] = g(rng); base[13] = g(rng); base[14] = g(rng); base[15] = 1;
		RandRot(rng, R);
		for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) pal[c * 4 + r] = R[c][r];
		pal[12] = g(rng); pal[13] = g(rng); pal[14] = g(rng); pal[15] = 1;
		Detail::Inverse4(base, inv);
		PaletteToGlobal(pal, base, G);
		GlobalToPalette(G, inv, back);
		for (int k = 0; k < 16; k++) worst = fmax(worst, fabs((double)back[k] - pal[k]));
	}
	printf("palette -> global -> palette (VRRig_PaletteToGlobal / VRRig_GlobalToPalette, swapYZ): max %.3g\n", worst);
	CHECK(worst < 1e-5, "palette round trip %.3g", worst);
}

int main()
{
	TestSmoothing();
	TestRule();
	TestPoseTarget();
	TestTurn();
	TestPalette();
	printf("test_curls: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
