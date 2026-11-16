// tools/handframe_test/test_handframe.cpp -- THE HAND FRAME, OUTSIDE THE ENGINE (2026-11-16).
//
// The owner, in the headset: lasers pointing behind him or off to the side, sprites floating through his
// hand and orbiting it as it moves. This proves, with numbers, what the readers did and what they do now.
// It compiles the engine's OWN src/r_data/vr_posemath.h (the device's grip and aim arithmetic, which
// vk_openxrdevice.cpp now calls) and src/r_data/vr_handpose_math.h (DefaultHandFit), and VSMatrix itself
// (src/common/utility/matrix.cpp). Nothing of the new path is copied.
//
// The OLD path is copied VERBATIM from the lane tip before the change (4a573a7d), each piece pinned by
// file:line, so the before/after numbers are the code's, not a summary's:
//   OLD_OffsetToWorld      vk_openxrdevice.cpp:7369-7387   (the device's grip, before the refactor)
//   OLD_GripOffset         vk_openxrdevice.cpp:4067-4074
//   OLD_AimAngles          vk_openxrdevice.cpp:797-810, 1228-1237, 4021-4029
//   OLD published fields   vk_openxrdevice.cpp:5304-5310 (main), 5366-5370 (off)
//   OLD_HandFrame          g_levellocals.h:470-483 (BillboardHandFrame) + :492-507 (BillboardHandAxes)
//   OLD_RayUseByHand       roth_runtime.cpp:7409-7413 / roth_interface.cpp:495-498 (dir z = -sin(AttackPitch))
//   OLD_RayFallback        roth_vrpick.cpp:459-470 (the same)
//
// Checks:
//   A. the refactor is exact: VRPose_OffsetToWorld / VRPose_HeadRelativeOffset / VRPose_AimAngles give the
//      device's old results bit for bit, over a grid of poses
//   B. the new frame IS the drawn hand's: for poses over a grid (player turned, head turned, both hands,
//      yaw / pitch / roll), a point fixed on the hand (its local coordinates in grip x DefaultHandFit) stored
//      in the frame at one pose resolves, after the hand moves and turns, onto that same point of the hand
//      (no orbit), and the ray along the frame equals the drawn finger's direction (a straight finger along
//      the hand's X)
//   C. the old path, the same motions: how far the old lock put the point from the hand, and how far the old
//      readers' ray was from the finger (printed; it is the fault, so it is expected to be large)
//
//   tools/handframe_test/run.sh

#include "vr_posemath.h"
#include "vr_handpose_math.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double D2R = M_PI / 180.0, R2D = 180.0 / M_PI;

//---------------------------------------------------------------------------
// THE OLD PATH, verbatim (see the header for the pins). Engine globals become parameters.
//---------------------------------------------------------------------------

struct XrQuaternionf { float x, y, z, w; };
struct XrVector3f { float x, y, z; };

// vk_openxrdevice.cpp:797-810
static XrVector3f OpenVREulerAnglesFromQuaternion(const XrQuaternionf& quat)
{
	double q0 = quat.w;
	// Permute axes to match OpenVR's yaw/pitch/roll convention.
	double q2 = quat.x;
	double q3 = quat.y;
	double q1 = quat.z;

	double roll = std::atan2(2.0 * (q0 * q1 + q2 * q3), 1.0 - 2.0 * (q1 * q1 + q2 * q2));
	double pitch = std::asin(2.0 * (q0 * q2 - q3 * q1));
	double yaw = std::atan2(2.0 * (q0 * q3 + q1 * q2), 1.0 - 2.0 * (q2 * q2 + q3 * q3));

	return { (float)yaw, (float)pitch, (float)roll };
}

// vk_openxrdevice.cpp:1228-1237
static double NormalizeAngle(double angle)
{
	angle = fmod(angle, 360.0);
	angle = fmod(angle + 360.0, 360.0);
	if (angle > 180.0)
	{
		angle -= 360.0;
	}
	return angle;
}

enum { PITCH = 0, YAW = 1, ROLL = 2 };

// vk_openxrdevice.cpp:4021-4029 (inside updateHandPose)
static void OLD_AimAngles(const XrQuaternionf &orientation, float vr_weaponRotate, float *angles)
{
	// Apply vr_weaponRotate as a pure pitch-domain adjustment
	const XrVector3f euler = OpenVREulerAnglesFromQuaternion(orientation);
	angles[YAW] = (float)(euler.x * (180.0 / M_PI));
	// Match OpenVR pitch-adjust direction: negative vr_weaponRotate should tilt the weapon downward
	constexpr float openxrWeaponPitchBiasDeg = -40.0f;
	angles[PITCH] = -(float)(euler.y * (180.0 / M_PI)) - (vr_weaponRotate * 2.0f) + openxrWeaponPitchBiasDeg;
	// Apply extra 180-degree roll flip to match the expected OpenVR-facing baseline
	angles[ROLL] = (float)NormalizeAngle(-(float)(euler.z * (180.0 / M_PI)));
}

// vk_openxrdevice.cpp:4067-4074 (the grip loop)
static void OLD_GripOffset(const float pos[3], const float hmdPosition[3], float viewpointYaw, float hmdYaw, float out[3])
{
	float go[3] = { pos[0] - hmdPosition[0],
	                pos[1] - hmdPosition[1],
	                pos[2] - hmdPosition[2] };
	const float gYaw = viewpointYaw - hmdYaw;
	DVector2 gRot = DVector2(go[0], go[2]).Rotated(-gYaw);
	out[0] = (float)gRot.Y;
	out[1] = go[1];
	out[2] = (float)gRot.X;
}

// vk_openxrdevice.cpp:7369-7387 (OffsetToWorld)
static void OLD_OffsetToWorld(const float ofs[3], const float rotXR[4], VSMatrix* mat, const DVector3 &CenterEyePos, float playerHeight,
	float upm, double pixelstretch, float hmdHeight, float vr_height_adjust, float doomYaw, float hmdYaw)
{
	mat->loadIdentity();
	mat->translate((float)CenterEyePos.X,
	               (float)CenterEyePos.Z - playerHeight,
	               (float)CenterEyePos.Y);
	mat->scale((float)upm, (float)upm, (float)-upm);
	mat->translate(-ofs[0], (hmdHeight + ofs[1] + (float)vr_height_adjust) / (float)pixelstretch, ofs[2]);
	mat->scale(1, 1 / (float)pixelstretch, 1);
	mat->rotate(-90 + doomYaw - hmdYaw, 0, 1, 0);
	mat->multQuaternion(FVector4(rotXR[0], rotXR[1], rotXR[2], rotXR[3]));
}

// g_levellocals.h:492-507 (BillboardHandAxes) -- unchanged by this work; the old BillboardHandFrame fed it
static void BillboardHandAxes(double yaw, double pitch, double roll, DVector3 &forward, DVector3 &right, DVector3 &up)
{
	const double DEG2RAD = 0.01745329251994329576923690768489;
	const double cy = cos(yaw * DEG2RAD), sy = sin(yaw * DEG2RAD);
	const double cp = cos(pitch * DEG2RAD), sp = sin(pitch * DEG2RAD);

	forward = DVector3(cy * cp, sy * cp, sp);
	const DVector3 flatRight(sy, -cy, 0.0);
	const DVector3 levelUp = flatRight ^ forward;

	const double cr = cos(roll * DEG2RAD), sr = sin(roll * DEG2RAD);
	right = flatRight * cr + levelUp * sr;
	up    = levelUp * cr - flatRight * sr;
}

// The pawn fields the device published (vk_openxrdevice.cpp:5304-5310, 5366-5370, outside the screen layer):
//   AttackPitch = -angles[PITCH]; AttackAngle = -90 + viewYaw + (angles[YAW] - playerYaw); MainHandRoll = angles[ROLL]
struct Published { DVector3 pos; double angle, pitch, roll; };

// g_levellocals.h:470-483 BillboardHandFrame (old): yaw = Angle + 90, pitch = -Pitch, roll = MainHandRoll
static void OLD_HandFrame(const Published &p, DVector3 &o, DVector3 &f, DVector3 &r, DVector3 &u)
{
	o = p.pos;
	const double yaw = p.angle + 90.0, pitch = -p.pitch, roll = p.roll;
	BillboardHandAxes(yaw, pitch, roll, f, r, u);
}

// roth_runtime.cpp:7409-7413 UseByHand / roth_interface.cpp PointerRays / roth_vrpick.cpp fallback (old)
static DVector3 OLD_Ray(const Published &p)
{
	const double yaw = (p.angle + 90.0) * D2R, pitch = p.pitch * D2R;
	return DVector3(cos(yaw) * cos(pitch), sin(yaw) * cos(pitch), -sin(pitch));
}

//---------------------------------------------------------------------------
// A pose of the world and the controllers
//---------------------------------------------------------------------------

struct World
{
	DVector3 eye = DVector3(512, -300, 41 + 64);
	float playerHeight = 41.f, upm = 107.4f, ps = 1.0f;   // a Realms map: the eye anchor's metre, square pixels
	float bodyYaw = 0.f, hmdYaw = 0.f;
	float hmd[3] = { 0.f, 0.f, 0.f };
};

struct Hand { float pos[3]; float grip[4]; };

static FVRPoseWorld WorldOf(const World &w)
{
	FVRPoseWorld W;
	W.centerEye = w.eye;
	W.playerHeight = w.playerHeight;
	W.unitsPerMetre = w.upm;
	W.pixelStretch = w.ps;
	W.hmdHeight = w.hmd[1];
	W.heightAdjust = 0.f;
	W.bodyYaw = w.bodyYaw;
	W.hmdYaw = w.hmdYaw;
	return W;
}

static void QMul(const float a[4], const float b[4], float o[4])
{
	o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// grip x DefaultHandFit: the drawn hand's frame (VRHandPose_HandFrame), through the NEW arithmetic
static VSMatrix HandMatrix(const World &w, const Hand &h, bool right)
{
	float ofs[3];
	VRPose_HeadRelativeOffset(h.pos, w.hmd, w.bodyYaw, w.hmdYaw, ofs);
	VSMatrix m;
	VRPose_OffsetToWorld(&m, WorldOf(w), ofs, h.grip);
	float fit[16];
	VRHandPose::DefaultHandFit(right, fit);
	m.multMatrix(fit);
	return m;
}

// The device's published aim for a hand (the aim = the grip turned so its -Z runs along the fingers, as
// roth_xrpose's default; a real controller's aim differs from its grip by a fixed turn, which only adds to
// the old path's error).
static Published PublishedFor(const World &w, const Hand &h)
{
	float rx[4], aim[4], angles[3], ofs[3];
	VRPose_QuatFromYPR(0, -90, 0, rx);
	QMul(h.grip, rx, aim);
	VRPose_AimAngles(aim, 0.f, angles);
	Published p;
	VRPose_PublishedAim(angles, w.bodyYaw, w.hmdYaw, p.angle, p.pitch, p.roll);
	VRPose_HeadRelativeOffset(h.pos, w.hmd, w.bodyYaw, w.hmdYaw, ofs);
	VSMatrix m;
	VRPose_OffsetToWorld(&m, WorldOf(w), ofs, aim);
	p.pos = VRPose_LocalToMap(m, 0, 0, 0);
	return p;
}

static Hand MakeHand(const World &w, double x, double y, double z, double yaw, double pitch, double wristRoll)
{
	// roth_xrpose `point`: the head's frame, the grip pitched +90 so the fingers (-Y_grip) run ahead
	Hand h;
	const double hy = w.hmdYaw * D2R;
	h.pos[0] = w.hmd[0] + (float)(x * cos(hy) + z * sin(hy));
	h.pos[1] = w.hmd[1] + (float)y;
	h.pos[2] = w.hmd[2] + (float)(-x * sin(hy) + z * cos(hy));
	float qy[4], qp[4], qr[4], t[4];
	VRPose_QuatFromYPR(w.hmdYaw + yaw, 0, 0, qy);
	VRPose_QuatFromYPR(0, 90.0 + pitch, 0, qp);
	VRPose_QuatFromYPR(wristRoll, 0, 0, qr);
	QMul(qy, qp, t);
	QMul(t, qr, h.grip);
	return h;
}

static double Ang(const DVector3 &a, const DVector3 &b)
{
	double c = (a | b) / (a.Length() * b.Length());
	c = c > 1 ? 1 : (c < -1 ? -1 : c);
	return acos(c) * R2D;
}

//---------------------------------------------------------------------------
// A. the refactor is exact
//---------------------------------------------------------------------------

static void TestRefactorExact()
{
	int n = 0;
	for (int i = 0; i < 400; i++)
	{
		// a deterministic spread of poses
		const double a = i * 0.731, b = i * 1.379, c = i * 2.113;
		float q[4];
		VRPose_QuatFromYPR(fmod(a * 97, 360) - 180, fmod(b * 53, 180) - 90, fmod(c * 71, 360) - 180, q);
		const float pos[3] = { (float)sin(a) * 0.6f, 1.2f + (float)cos(b) * 0.5f, (float)sin(c) * 0.6f };
		const float hmd[3] = { (float)cos(a) * 0.2f, 1.6f, (float)sin(b) * 0.2f };
		const float bodyYaw = (float)fmod(a * 131, 360), hmdYaw = (float)fmod(b * 77, 360) - 180;
		const float rot = (float)fmod(c, 10) - 5;

		float o1[3], o2[3];
		OLD_GripOffset(pos, hmd, bodyYaw, hmdYaw, o1);
		VRPose_HeadRelativeOffset(pos, hmd, bodyYaw, hmdYaw, o2);
		CHECK(memcmp(o1, o2, sizeof o1) == 0, "grip offset differs at pose %d", i);

		VSMatrix m1, m2;
		const DVector3 eye(100 + i, -50 - i, 90 + (i % 7));
		const double ps = (i & 1) ? 1.2 : 1.0;
		OLD_OffsetToWorld(o1, q, &m1, eye, 41.f, 107.4f, ps, hmd[1], 0.05f, bodyYaw, hmdYaw);
		FVRPoseWorld W;
		W.centerEye = eye; W.playerHeight = 41.f; W.unitsPerMetre = 107.4f; W.pixelStretch = (float)ps;
		W.hmdHeight = hmd[1]; W.heightAdjust = 0.05f; W.bodyYaw = bodyYaw; W.hmdYaw = hmdYaw;
		VRPose_OffsetToWorld(&m2, W, o2, q);
		CHECK(memcmp(m1.get(), m2.get(), 16 * sizeof(FLOATTYPE)) == 0, "OffsetToWorld differs at pose %d", i);

		float a1[3], a2[3];
		OLD_AimAngles(XrQuaternionf{ q[0], q[1], q[2], q[3] }, rot, a1);
		VRPose_AimAngles(q, rot, a2);
		CHECK(memcmp(a1, a2, sizeof a1) == 0, "aim angles differ at pose %d: (%g %g %g) vs (%g %g %g)", i, a1[0], a1[1], a1[2], a2[0], a2[1], a2[2]);
		n++;
	}
	printf("A. the device's grip offset, OffsetToWorld and aim angles: the shared functions equal the old code bit for bit on %d poses\n", n);
}

//---------------------------------------------------------------------------
// B and C. a point on the hand, a ray along the finger, over motions
//---------------------------------------------------------------------------

struct Motion { double dx, dy, dz, dyaw, dpitch, droll; const char *what; };

static void TestFrameRidesHand()
{
	const Motion motions[] = {
		{ 0, 0, 0, 0, 30, 0, "the wrist tipped up 30 deg" },
		{ 0, 0, 0, 0, -45, 0, "tipped down 45 deg" },
		{ 0, 0, 0, 40, 0, 0, "turned left 40 deg" },
		{ 0, 0, 0, 0, 0, 90, "rolled palm-out 90 deg" },
		{ 0.15, 0.10, -0.20, 25, 20, -60, "moved 27 cm and turned three ways" },
		{ -0.3, -0.2, 0.1, -70, -30, 120, "swung across the body" },
	};
	const double bodyYaws[] = { 0, 90, 217, -45 };
	const double hmdYaws[] = { 0, 35, -120 };
	const double startPitch[] = { 0, 25, -40 };
	// points fixed on the hand, in the hand frame (metres): a card above the palm, the fingertip, a beam dot 2 m out
	const double pts[][3] = { { 0.02, 0.05, -0.03 }, { 0.17, 0.0, 0.0 }, { 2.0, 0.0, 0.0 } };

	double newWorst = 0, newRayWorst = 0, oldWorst = 0, oldRayWorst = 0, oldRayBest = 1e9;
	int cases = 0;
	for (double by : bodyYaws) for (double hy : hmdYaws) for (double sp : startPitch) for (int right = 0; right < 2; right++)
	for (const Motion &mv : motions)
	{
		World w;
		w.bodyYaw = (float)by; w.hmdYaw = (float)hy;
		w.hmd[1] = (float)(w.playerHeight * w.ps / w.upm);
		const double side = right ? 0.2 : -0.2;
		const Hand h1 = MakeHand(w, side, -0.25, -0.35, 10.0 * (right ? -1 : 1), sp, 0);
		const Hand h2 = MakeHand(w, side + mv.dx, -0.25 + mv.dy, -0.35 + mv.dz, 10.0 * (right ? -1 : 1) + mv.dyaw, sp + mv.dpitch, mv.droll);
		const VSMatrix M1 = HandMatrix(w, h1, right), M2 = HandMatrix(w, h2, right);

		DVector3 o1, f1, r1, u1, o2, f2, r2, u2;
		const bool ok1 = VRPose_HandMapFrame(M1, right, o1, f1, r1, u1);
		const bool ok2 = VRPose_HandMapFrame(M2, right, o2, f2, r2, u2);
		CHECK(ok1 && ok2, "no frame");
		// orthonormal and right-handed
		CHECK(fabs(f1.Length() - 1) < 1e-9 && fabs(r1.Length() - 1) < 1e-9 && fabs(u1.Length() - 1) < 1e-9, "frame not unit");
		CHECK(fabs(f1 | r1) < 1e-9 && fabs(f1 | u1) < 1e-9 && fabs(r1 | u1) < 1e-9, "frame not square");
		CHECK(((f1 ^ r1) | u1) < 0, "frame handedness: fwd x right should point DOWN (-up) in the map's right-handed axes");
		// the thumb's side: up is -Z_hand for the right hand, +Z_hand for the left (vr_handpose_math.h DefaultHandFit)
		const DVector3 zw = VRPose_LocalToMap(M1, 0, 0, 1) - o1;
		CHECK(((zw / zw.Length()) | u1) * (right ? -1 : 1) > 0.999, "%s hand: up is not the thumb's side", right ? "right" : "left");
		// BillboardHandFrame's angles rebuild the same basis through BillboardHandAxes
		double yy, pp, rr;
		VRPose_FrameAngles(f1, r1, yy, pp, rr);
		DVector3 fa, ra, ua;
		BillboardHandAxes(yy, pp, rr, fa, ra, ua);
		CHECK((fa - f1).Length() < 1e-9 && (ra - r1).Length() < 1e-9 && (ua - u1).Length() < 1e-9, "angles do not rebuild the frame");

		// the published aim (old readers' source) at both poses
		const Published p1 = PublishedFor(w, h1), p2 = PublishedFor(w, h2);
		DVector3 oo1, of1, or1, ou1, oo2, of2, or2, ou2;
		OLD_HandFrame(p1, oo1, of1, or1, ou1);
		OLD_HandFrame(p2, oo2, of2, or2, ou2);

		for (const auto &pt : pts)
		{
			// the true point on the drawn hand at each pose
			const DVector3 w1 = VRPose_LocalToMap(M1, pt[0], pt[1], pt[2]);
			const DVector3 w2 = VRPose_LocalToMap(M2, pt[0], pt[1], pt[2]);
			// NEW: stored in the frame at pose 1 (LockFor), resolved at pose 2 (the renderer)
			const DVector3 d = w1 - o1;
			const DVector3 L(d | f1, d | r1, d | u1);
			const DVector3 n2 = o2 + f2 * L.X + r2 * L.Y + u2 * L.Z;
			const double en = (n2 - w2).Length() / w.upm;
			if (en > newWorst) newWorst = en;
			// OLD
			const DVector3 od = w1 - oo1;
			const DVector3 OL(od | of1, od | or1, od | ou1);
			const DVector3 m2 = oo2 + of2 * OL.X + or2 * OL.Y + ou2 * OL.Z;
			const double eo = (m2 - w2).Length() / w.upm;
			if (eo > oldWorst) oldWorst = eo;
		}
		// the finger: a straight finger lies along the hand's X; drawn knuckle and tip through the hand matrix
		const DVector3 knuckle = VRPose_LocalToMap(M2, 0.09, 0, 0), tip = VRPose_LocalToMap(M2, 0.17, 0, 0);
		const double rn = Ang(f2, tip - knuckle);
		if (rn > newRayWorst) newRayWorst = rn;
		const double ro = Ang(OLD_Ray(p2), tip - knuckle);
		if (ro > oldRayWorst) oldRayWorst = ro;
		if (ro < oldRayBest) oldRayBest = ro;
		cases++;
	}
	printf("B. NEW, %d poses (4 player yaws x 3 head yaws x 3 start pitches x both hands x 6 motions, Realms' metre, square pixels):\n"
		"   a point fixed on the hand (a card, the fingertip, a beam dot 2 m out), locked at one pose and resolved after the motion:\n"
		"   worst %.6f mm from that point of the drawn hand (float matrices); the hand frame's ray vs the drawn finger: worst %.6f deg\n",
		cases, newWorst * 1000.0, newRayWorst);
	printf("C. OLD (the aim pose's fields through the old BillboardHandFrame and the old readers' ray), the same motions:\n"
		"   the locked point ended up to %.1f cm from where it belongs on the hand; the old ray was %.1f to %.1f deg off the finger\n",
		oldWorst * 100.0, oldRayBest, oldRayWorst);
	CHECK(newWorst < 1e-5, "NEW: a hand-locked point leaves the hand by %.6f mm", newWorst * 1000.0);
	CHECK(newRayWorst < 1e-4, "NEW: the ray is %.6f deg off the drawn finger", newRayWorst);
	CHECK(oldWorst > 0.05, "OLD path did not reproduce the orbit (%.3f m) -- the before/after comparison is not measuring the fault", oldWorst);

	// With the engine's default pixel stretch (1.2, not a Realms map) the mapped axes are a hair off square;
	// the squared frame then drifts from the stretched hand by a small amount: measured, not assumed.
	double stretchWorst = 0;
	for (double by : bodyYaws) for (int right = 0; right < 2; right++) for (const Motion &mv : motions)
	{
		World w; w.ps = 1.2f; w.upm = 34.f; w.bodyYaw = (float)by;
		w.hmd[1] = (float)(w.playerHeight * w.ps / w.upm);
		const Hand h1 = MakeHand(w, 0.2, -0.25, -0.35, 0, 0, 0);
		const Hand h2 = MakeHand(w, 0.2 + mv.dx, -0.25 + mv.dy, -0.35 + mv.dz, mv.dyaw, mv.dpitch, mv.droll);
		const VSMatrix M1 = HandMatrix(w, h1, right), M2 = HandMatrix(w, h2, right);
		DVector3 o1, f1, r1, u1, o2, f2, r2, u2;
		VRPose_HandMapFrame(M1, right, o1, f1, r1, u1);
		VRPose_HandMapFrame(M2, right, o2, f2, r2, u2);
		const DVector3 w1 = VRPose_LocalToMap(M1, 0.02, 0.05, -0.03), w2 = VRPose_LocalToMap(M2, 0.02, 0.05, -0.03);
		const DVector3 d = w1 - o1;
		const DVector3 n2 = o2 + f2 * (d | f1) + r2 * (d | r1) + u2 * (d | u1);
		const double e = (n2 - w2).Length() / w.upm;
		if (e > stretchWorst) stretchWorst = e;
	}
	printf("   (Doom's 1.2 pixel stretch, a card 6 cm from the grip: worst %.2f mm -- the stretched hand is not a rigid body)\n", stretchWorst * 1000.0);
	CHECK(stretchWorst < 0.02, "with pixel stretch 1.2 the drift is %.1f mm", stretchWorst * 1000.0);
}

int main()
{
	TestRefactorExact();
	TestFrameRidesHand();
	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
