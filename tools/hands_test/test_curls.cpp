// tools/hands_test/test_curls.cpp -- DIFFERENTIAL TEST of the VR hands' curl rule, its smoothing and the turn on
// the joints, against UZDXREMA's OWN CODE.
//
// run.sh cuts these lines out of UZDXREMA (read-only, HEAD 39e4dad) and they are #included below, unedited:
//   vr_avatar_pose.cpp:336-348   FromMat3Pos
//   vr_avatar_pose.cpp:392-414   QSlerp, QAxisAngle
//   vr_avatar_pose.cpp:429-431   the rate (dt sanitised, retain = 0.75^(dt*90))
//   vr_avatar_pose.cpp:440-464   the curl rule, empty hand (index/trigger+touch, grip, thumb touch, no controller)
//   vr_avatar_pose.cpp:658-696   the scalar smoothing, the rotation targets (QAxisAngle of the table), gRot's slerp
//   vr_avatar_pose.cpp:1785-1836 the turn D = F.R(q).F^T on the joint globals and the carry of the finger
//   vr_armik.h:35-54             FVRMat3
// The only things supplied here are the engine names those lines use (a hand state, a rig with Valid(), the table
// lookup VRAvatar_Curl, a VSMatrix with multMatrix/inverseMatrix, FVector3). Lines 466-656 (a hand holding a weapon
// or an object) are not cut: with nothing held, the source runs none of them.
//
// The port (src/r_data/vr_handpose_math.h) and the source are fed the same inputs, frame by frame, at 72, 80, 90,
// 120 and 144 Hz, with jittered and hitched frame times, both sides, controllers dropping out; every scalar curl and
// every smoothed quaternion is compared every frame. Then the port's FBX-frame target (QSlerp(open, fist, curl), the
// owner's directive) against the source's own QSlerp on the same ends. Then the turn on random skeletons.
//
//   ./test_curls <vrhandcurl.txt>

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

	struct FVRHandCurlJoint { float Degrees = 0.f; FVector3 Axis = FVector3(0, 0, 1); };
	struct FString { bool IsEmpty() const { return true; } const char *GetChars() const { return ""; } };
	struct FCurlState { float Curl[VRFINGER_COUNT][VRFINGER_JOINTS] = {}; };
	struct FRotState { float Q[VRFINGER_COUNT][VRFINGER_JOINTS][4] = {}; bool Init = false; };
	struct FVRRigHandState { float trigger = 0, gripValue = 0; bool triggerTouch = false, thumbTouch = false; };
	struct FRig { bool valid[2] = { true, true }; bool Valid(int i) const { return valid[i]; } };
	struct FVRAvatarFingerJoint { int Joint = -1; FVector3 Frame[3] = { FVector3(1, 0, 0), FVector3(0, 1, 0), FVector3(0, 0, 1) }; };
	struct FVRAvatarSide { FVRAvatarFingerJoint Fingers[VRFINGER_COUNT][VRFINGER_JOINTS]; };

	const FVRHandCurlJoint *gTable = nullptr;	// [5*3] or null (table not loaded)
	const FVRHandCurlJoint *VRAvatar_Curl(int f, int j) { return gTable ? &gTable[f * 3 + j] : nullptr; }
	bool VRAvatar_NamedPoseRot(const char *, int, int, int, float *) { return false; }

	FCurlState gCurl[2];
	FRotState gRot[2];

	float Retain(double dt)
	{
#include "retain.inc"
		return retain;
	}

	// one side of UpdateCurls, nothing held
	void Step(int sd, bool isMain, const FVRRigHandState &h, const FRig &rig, float retain)
	{
#include "rule.inc"
#include "smooth.inc"
	}

	void Turn(int sd, const FVRAvatarSide &side, std::vector<VSMatrix> &gGlobals)
	{
		const int n = (int)gGlobals.size();
#include "turn.inc"
	}
}

// ======================================================================= the table
static bool ReadTable(const char *path, VRHandPose::CurlJoint port[5][3], UZ::FVRHandCurlJoint uz[15])
{
	FILE *f = fopen(path, "r");
	if (!f) { printf("cannot open %s\n", path); return false; }
	static const char *names[5] = { "thumb", "index", "middle", "ring", "pinky" };
	char line[512];
	int seen = 0;
	while (fgets(line, sizeof(line), f))
	{
		char fn[32]; int j; double deg, ax, ay, az;
		if (sscanf(line, " curl %31s %d %lf %lf %lf %lf", fn, &j, &deg, &ax, &ay, &az) != 6) continue;
		int fi = -1;
		for (int k = 0; k < 5; k++) if (!strcmp(fn, names[k])) fi = k;
		if (fi < 0 || j < 1 || j > 3) continue;
		// as VA LoadCurlTable :252-261: the axis divided by its length, degrees as read
		const float axf[3] = { (float)ax, (float)ay, (float)az };
		const float len = sqrtf(axf[0] * axf[0] + axf[1] * axf[1] + axf[2] * axf[2]);
		for (int k = 0; k < 3; k++) port[fi][j - 1].Axis[k] = axf[k] / len;
		port[fi][j - 1].Degrees = (float)deg;
		uz[fi * 3 + j - 1].Axis = UZ::FVector3(axf[0] / len, axf[1] / len, axf[2] / len);
		uz[fi * 3 + j - 1].Degrees = (float)deg;
		seen++;
	}
	fclose(f);
	return seen == 15;
}

// ======================================================================= 1. the rule and the smoothing
static double QDiff(const float a[4], const float b[4])
{
	double d = 0;
	for (int k = 0; k < 4; k++) d = fmax(d, fabs((double)a[k] - b[k]));
	return d;
}

static void TestRule(const VRHandPose::CurlJoint table[5][3], const UZ::FVRHandCurlJoint *uzTable)
{
	using namespace VRHandPose;
	std::mt19937 rng(20261113);
	std::uniform_real_distribution<float> u01(0.f, 1.f);
	const double rates[] = { 72, 80, 90, 120, 144 };
	for (int withTable = 0; withTable < 2; withTable++)
	{
		for (double hz : rates)
		{
			UZ::gTable = withTable ? uzTable : nullptr;
			for (int s = 0; s < 2; s++) { UZ::gCurl[s] = UZ::FCurlState(); UZ::gRot[s] = UZ::FRotState(); }
			CurlState pc[2]; SideState pr[2];
			double worstCurl = 0, worstQ = 0, worstRetain = 0;
			int frames = 0;
			for (int fr = 0; fr < 3000; fr++)
			{
				// frame time: the rate, jittered 10%; every 400th a hitch (0.3 s), every 777th a repeat (0)
				double dt = (1.0 / hz) * (0.9 + 0.2 * u01(rng));
				if (fr % 400 == 399) dt = 0.3;
				if (fr % 777 == 776) dt = 0.0;
				const float rp = Retain(dt), ru = UZ::Retain(dt);
				worstRetain = fmax(worstRetain, fabs((double)rp - ru));

				for (int sd = 0; sd < 2; sd++)
				{
					// inputs that hold for a while and then jump (presses, releases, touches), and drift
					UZ::FVRRigHandState h;
					UZ::FRig rig;
					const int phase = (fr / 37 + sd * 3) % 11;
					h.trigger = (phase % 3 == 0) ? 0.f : (phase % 3 == 1 ? 1.f : u01(rng));
					h.gripValue = (phase % 4 == 0) ? 0.f : (phase % 4 == 1 ? 1.f : u01(rng));
					h.triggerTouch = (phase % 2) == 0 || h.trigger > 0.f;
					h.thumbTouch = (phase % 5) < 2;
					const bool valid = !((fr / 151) % 7 == 3);	// a dropout now and then
					const bool isMain = (sd == 0);	// right-handed: the right side holds the main controller
					rig.valid[isMain ? UZ::VRRIG_MAIN : UZ::VRRIG_OFF] = valid;
					UZ::Step(sd, isMain, h, rig, ru);

					Inputs in;
					in.trigger = h.trigger; in.grip = h.gripValue; in.triggerTouch = h.triggerTouch; in.thumbTouch = h.thumbTouch;
					in.valid = valid;
					StepSide(pc[sd], pr[sd], in, rp, nullptr, withTable ? table : nullptr);

					for (int f = 0; f < 5; f++)
						for (int j = 0; j < 3; j++)
						{
							worstCurl = fmax(worstCurl, fabs((double)pc[sd].Curl[f][j] - UZ::gCurl[sd].Curl[f][j]));
							worstQ = fmax(worstQ, QDiff(pr[sd].Q[f][j], UZ::gRot[sd].Q[f][j]));
						}
				}
				frames++;
			}
			printf("rule+smoothing, %s, %3.0f Hz: %d frames x 2 sides x 15 joints: max |curl - source| %.3g, max |q - source| %.3g, max |retain - source| %.3g\n",
				withTable ? "FRIK table loaded" : "no table (identity)", hz, frames, worstCurl, worstQ, worstRetain);
			CHECK(worstCurl == 0.0 && worstQ == 0.0 && worstRetain == 0.0, "port differs from UZDXREMA's own lines at %.0f Hz (curl %.3g, q %.3g, retain %.3g)", hz, worstCurl, worstQ, worstRetain);
		}
	}

	// The rule's values, said out loud (VAP :444-454), through the port.
	struct Case { const char *name; float grip, trigger; bool tt, th, valid; float want[5]; };
	const Case cases[] =
	{
		{ "open",          0, 0, false, false, true,  { 0.f, 0.f, 0.f, 0.f, 0.f } },
		{ "fist",          1, 1, true,  true,  true,  { 0.6f, 1.f, 1.f, 1.f, 1.f } },
		{ "point",         1, 0, false, true,  true,  { 0.6f, 0.f, 1.f, 1.f, 1.f } },
		{ "thumb up",      1, 1, true,  false, true,  { 0.f, 1.f, 1.f, 1.f, 1.f } },
		{ "trigger only",  0, 1, true,  false, true,  { 0.f, 1.f, 0.f, 0.f, 0.f } },
		{ "trigger touch", 0, 0, true,  false, true,  { 0.f, 0.3f, 0.f, 0.f, 0.f } },
		{ "no controller", 1, 1, true,  true,  false, { 0.f, 0.f, 0.f, 0.f, 0.f } },
	};
	for (const Case &c : cases)
	{
		Inputs in; in.grip = c.grip; in.trigger = c.trigger; in.triggerTouch = c.tt; in.thumbTouch = c.th; in.valid = c.valid;
		float pf[5];
		FingerCurls(in, pf);
		bool ok = true;
		for (int f = 0; f < 5; f++) ok = ok && pf[f] == c.want[f];
		printf("  %-14s thumb %.1f index %.1f middle %.1f ring %.1f pinky %.1f\n", c.name, pf[0], pf[1], pf[2], pf[3], pf[4]);
		CHECK(ok, "rule: %s", c.name);
	}
}

// ======================================================================= 2. the FBX-frame target
// The port's target with FBX poses is QSlerp(open, fist, curl) (the source's [POSEROT] blend, VAP :680, with the
// owner's two ends); its smoothing is the source's gRot slerp (:691-693). Replayed here with the source's own QSlerp.
static void TestPoseTarget()
{
	using namespace VRHandPose;
	std::mt19937 rng(7);
	std::normal_distribution<float> g(0.f, 1.f);
	std::uniform_real_distribution<float> u01(0.f, 1.f);
	JointPose poses[5][3];
	for (auto &pf : poses)
		for (auto &p : pf)
		{
			float a[4] = { g(rng), g(rng), g(rng), g(rng) }, b[4] = { g(rng), g(rng), g(rng), g(rng) };
			float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]), nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
			for (int k = 0; k < 4; k++) { p.Open[k] = a[k] / na; p.Fist[k] = b[k] / nb; }
			p.Have = true;
		}
	const double rates[] = { 72, 80, 90, 120, 144 };
	for (double hz : rates)
	{
		CurlState pc; SideState pr;
		UZ::FRotState ref;
		double worst = 0;
		for (int fr = 0; fr < 2000; fr++)
		{
			const double dt = (1.0 / hz) * (0.9 + 0.2 * u01(rng));
			const float retain = UZ::Retain(dt);
			Inputs in;
			const int phase = fr / 41 % 6;
			in.trigger = phase < 3 ? u01(rng) : 0.f; in.grip = phase % 2 ? 1.f : u01(rng);
			in.triggerTouch = phase != 4; in.thumbTouch = phase < 2; in.valid = true;
			StepSide(pc, pr, in, retain, poses, nullptr);

			// the source's formulas: want (rule), then QSlerp(open, fist, want), then gRot's slerp
			float pf[5];
			pf[UZ::VRFINGER_INDEX] = (in.trigger > 0.f) ? in.trigger : (in.triggerTouch ? 0.3f : 0.f);
			pf[UZ::VRFINGER_MIDDLE] = pf[UZ::VRFINGER_RING] = pf[UZ::VRFINGER_PINKY] = in.grip;
			pf[UZ::VRFINGER_THUMB] = in.thumbTouch ? 0.6f : 0.f;
			for (int f = 0; f < 5; f++)
				for (int j = 0; j < 3; j++)
				{
					float wq[4];
					UZ::QSlerp(poses[f][j].Open, poses[f][j].Fist, pf[f], wq);
					float *cur = ref.Q[f][j];
					if (!ref.Init) { for (int k = 0; k < 4; ++k) cur[k] = wq[k]; }
					else { float o[4]; UZ::QSlerp(cur, wq, 1.f - retain, o); for (int k = 0; k < 4; ++k) cur[k] = o[k]; }
					worst = fmax(worst, QDiff(cur, pr.Q[f][j]));
				}
			ref.Init = true;
		}
		printf("FBX-frame target + smoothing, %3.0f Hz: 2000 frames x 15 joints, max |q - source formula| %.3g\n", hz, worst);
		CHECK(worst == 0.0, "FBX-frame target differs at %.0f Hz (%.3g)", hz, worst);
	}
	// the two ends: curl 0 is the open pose, curl 1 the fist
	float q[4];
	JointTarget(&poses[1][0], nullptr, 0.f, q);
	CHECK(QDiff(q, poses[1][0].Open) < 1e-6, "curl 0 = open");
	JointTarget(&poses[1][0], nullptr, 1.f, q);
	const float *fq = poses[1][0].Fist;
	const double d = fabs(q[0] * fq[0] + q[1] * fq[1] + q[2] * fq[2] + q[3] * fq[3]);
	CHECK(d > 1 - 1e-6, "curl 1 = fist (|dot| %.7f)", d);
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

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: test_curls <vrhandcurl.txt>\n"); return 2; }
	VRHandPose::CurlJoint table[5][3];
	UZ::FVRHandCurlJoint uz[15];
	CHECK(ReadTable(argv[1], table, uz), "the table has all 15 curl lines");
	TestRule(table, uz);
	TestPoseTarget();
	TestTurn();
	TestPalette();
	printf("test_curls: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
