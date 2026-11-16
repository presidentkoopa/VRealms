/*
** vr_posemath.h
**
** RS FORK -- THE CONTROLLER POSE INTO THE WORLD, ONE COPY.
**
** VKOpenXRDeviceMode turns each controller's tracked GRIP pose into a world matrix in two steps:
** the position, made head-relative and turned by the body yaw (vk_openxrdevice.cpp, the grip loop
** after updateHandPose), and OffsetToWorld (the eye, the metres, the -Z, the height, the pixel
** stretch, the body yaw minus the headset's own, the raw quaternion). Both steps live HERE now and
** the device calls them, so the controller-pose test (roth_xrpose, r_data/vr_handpose.cpp) can drive
** the very same arithmetic from given poses on a desk, with no headset: what it proves about the
** hand frame is proved about the code the headset runs.
**
** Engine-free apart from VSMatrix and DVector2 (tools/handframe_test compiles it alone).
**
**---------------------------------------------------------------------------
*/

#pragma once

#include "vectors.h"
#include "matrix.h"

// What OffsetToWorld reads from the frame besides the pose itself.
struct FVRPoseWorld
{
	DVector3 centerEye;             // r_viewpoint.CenterEyePos, map units (x, y, height)
	float playerHeight = 0.f;       // GetDoomPlayerHeightWithoutCrouch, map units
	float unitsPerMetre = 34.f;     // VR_UnitsPerMeter()
	float pixelStretch = 1.2f;      // the level's pixelstretch
	float hmdHeight = 0.f;          // hmdPosition[1], metres above the tracking origin
	float heightAdjust = 0.f;       // vr_height_adjust, metres
	float bodyYaw = 0.f;            // doomYaw, degrees
	float hmdYaw = 0.f;             // hmdorientation[YAW], degrees
};

// A tracked position, metres in the tracking space, made head-relative and turned into the frame
// OffsetToWorld takes (vk_openxrdevice.cpp, the grip loop: go = pos - hmd; gRot = (go.x, go.z)
// rotated by -(bodyYaw - hmdYaw); ofs = (gRot.Y, go.y, gRot.X)).
inline void VRPose_HeadRelativeOffset(const float pos[3], const float hmd[3], float bodyYaw, float hmdYaw, float ofs[3])
{
	const float go[3] = { pos[0] - hmd[0], pos[1] - hmd[1], pos[2] - hmd[2] };
	const float gYaw = bodyYaw - hmdYaw;
	const DVector2 gRot = DVector2(go[0], go[2]).Rotated(-gYaw);
	ofs[0] = (float)gRot.Y;
	ofs[1] = go[1];
	ofs[2] = (float)gRot.X;
}

// OffsetToWorld, step for step (vk_openxrdevice.cpp, ported from UZDXREMA vk_openxrdevice.cpp:7876-7921).
// rotXR is the raw OpenXR quaternion (x, y, z, w).
inline void VRPose_OffsetToWorld(VSMatrix *mat, const FVRPoseWorld &W, const float ofs[3], const float rotXR[4])
{
	mat->loadIdentity();
	mat->translate((float)W.centerEye.X, (float)W.centerEye.Z - W.playerHeight, (float)W.centerEye.Y);
	mat->scale(W.unitsPerMetre, W.unitsPerMetre, -W.unitsPerMetre);
	mat->translate(-ofs[0], (W.hmdHeight + ofs[1] + W.heightAdjust) / W.pixelStretch, ofs[2]);
	mat->scale(1, 1 / W.pixelStretch, 1);
	mat->rotate(-90 + W.bodyYaw - W.hmdYaw, 0, 1, 0);
	mat->multQuaternion(FVector4(rotXR[0], rotXR[1], rotXR[2], rotXR[3]));
}

// THE AIM POSE'S ANGLES (vk_openxrdevice.cpp updateHandPose: OpenVREulerAnglesFromQuaternion, then the
// pitch made Doom's (down-positive) with vr_weaponRotate and the -40 degree OpenXR pitch bias, the roll
// negated and wrapped). angles = { pitch, yaw, roll } in the device's PITCH/YAW/ROLL order (0, 1, 2).
inline void VRPose_AimAngles(const float q[4], float weaponRotate, float angles[3])
{
	const double q0 = q[3], q2 = q[0], q3 = q[1], q1 = q[2];   // the axes permuted to OpenVR's
	const double roll = atan2(2.0 * (q0 * q1 + q2 * q3), 1.0 - 2.0 * (q1 * q1 + q2 * q2));
	const double pitch = asin(2.0 * (q0 * q2 - q3 * q1));
	const double yaw = atan2(2.0 * (q0 * q3 + q1 * q2), 1.0 - 2.0 * (q2 * q2 + q3 * q3));
	const float ex = (float)yaw, ey = (float)pitch, ez = (float)roll;
	const double R2D = 180.0 / 3.14159265358979323846;
	constexpr float openxrWeaponPitchBiasDeg = -40.0f;
	angles[1] = (float)(ex * R2D);
	angles[0] = -(float)(ey * R2D) - (weaponRotate * 2.0f) + openxrWeaponPitchBiasDeg;
	double r = -(float)(ez * R2D);
	r = fmod(r, 360.0);
	r = fmod(r + 360.0, 360.0);
	if (r > 180.0) r -= 360.0;
	angles[2] = (float)r;
}

// WHAT THE DEVICE PUBLISHES ON THE PAWN from those angles (vk_openxrdevice.cpp, the gameplay block, outside
// the screen layer): AttackAngle / OffhandAngle = -90 + body yaw + (yaw - head yaw), the playsim's stored
// convention (world yaw less 90); AttackPitch / OffhandPitch = -pitch (positive UP); the roll as it is.
// The main hand's head yaw is the global playerYaw, which VR_SetHMDOrientation keeps equal to the head's.
inline void VRPose_PublishedAim(const float angles[3], float bodyYaw, float hmdYaw, double &angle, double &pitch, double &roll)
{
	angle = -90 + bodyYaw + (angles[1] - hmdYaw);
	pitch = -angles[0];
	roll = angles[2];
}

// A quaternion from yaw, pitch and roll in the OpenXR tracking space (+X right, +Y up, -Z ahead),
// applied yaw (about +Y, + turns left), then pitch (about +X, + tips -Z up), then roll (about +Z):
// q = Ry(yaw) * Rx(pitch) * Rz(roll). Only the test uses it; the device has its quaternion.
inline void VRPose_QuatFromYPR(double yawDeg, double pitchDeg, double rollDeg, float q[4])
{
	const double D2R = 3.14159265358979323846 / 180.0;
	const double hy = yawDeg * D2R * 0.5, hp = pitchDeg * D2R * 0.5, hr = rollDeg * D2R * 0.5;
	// qy = (0, sin hy, 0, cos hy), qp = (sin hp, 0, 0, cos hp), qr = (0, 0, sin hr, cos hr)
	auto mul = [](const double a[4], const double b[4], double o[4])
	{
		o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
		o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
		o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
		o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	};
	const double qy[4] = { 0, sin(hy), 0, cos(hy) }, qp[4] = { sin(hp), 0, 0, cos(hp) }, qr[4] = { 0, 0, sin(hr), cos(hr) };
	double t[4], r[4];
	mul(qy, qp, t);
	mul(t, qr, r);
	for (int i = 0; i < 4; i++) q[i] = (float)r[i];
}

// A point of a matrix's local space in MAP coordinates: the matrix is the renderer's (x, HEIGHT, y),
// a map position (x, y, z) -- the swap AActor::GetBonePosition and ModelWorldTransform make.
inline DVector3 VRPose_LocalToMap(const VSMatrix &m, double x, double y, double z)
{
	const FLOATTYPE *v = m.get();
	return DVector3(v[0] * x + v[4] * y + v[8] * z + v[12],
	                v[2] * x + v[6] * y + v[10] * z + v[14],
	                v[1] * x + v[5] * y + v[9] * z + v[13]);
}

// THE HAND'S FRAME IN THE MAP, from the hand frame matrix (grip x DefaultHandFit, vr_handpose.h
// VRHandPose_HandFrame): origin at the grip socket, `fwd` along the hand (X_hand, wrist to fingers),
// `right` to the hand's right as its owner sees it, `up` = right x fwd (the thumb's side).
//
// X_hand runs wrist -> knuckles; Y_hand is the back of the hand (the negated palm normal); the thumb is
// on -Z_hand for the right hand and +Z_hand for the left (vr_handpose_math.h). Held as a controller is
// held, the back of the RIGHT hand faces right and the back of the LEFT hand faces left, so `right` is
// +Y_hand for the right hand and -Y_hand for the left; `up` then comes out on the thumb's side for both
// (right: Y x X = -Z; left: -Y x X = +Z). Squared against `fwd`, because the pixel stretch (Y_gl /
// stretch) leaves the mapped axes a hair off square.
inline bool VRPose_HandMapFrame(const VSMatrix &hand, bool rightHand, DVector3 &origin, DVector3 &fwd, DVector3 &right, DVector3 &up)
{
	origin = VRPose_LocalToMap(hand, 0, 0, 0);
	DVector3 x = VRPose_LocalToMap(hand, 1, 0, 0) - origin;
	DVector3 y = VRPose_LocalToMap(hand, 0, 1, 0) - origin;
	const double xl = x.Length();
	if (xl < 1e-9) return false;
	fwd = x / xl;
	y = y - fwd * (y | fwd);
	const double yl = y.Length();
	if (yl < 1e-9) return false;
	right = (rightHand ? y : -y) / yl;
	up = right ^ fwd;
	return true;
}

// The angles BillboardHandAxes (g_levellocals.h) rebuilds that frame from: yaw and pitch (positive UP)
// of `fwd`, and the roll that turns BillboardHandAxes' level right onto `right`. Exact for any frame
// whose `fwd` is not straight up or down; there the yaw is taken from `right`.
inline void VRPose_FrameAngles(const DVector3 &fwd, const DVector3 &right, double &yaw, double &pitch, double &roll)
{
	const double R2D = 57.29577951308232087679815481410517;
	const double h = sqrt(fwd.X * fwd.X + fwd.Y * fwd.Y);
	pitch = atan2(fwd.Z, h) * R2D;
	yaw = h > 1e-9 ? atan2(fwd.Y, fwd.X) * R2D : atan2(-right.X, right.Y) * R2D;
	const double D2R = 1.0 / R2D;
	const double cy = cos(yaw * D2R), sy = sin(yaw * D2R);
	const DVector3 flatRight(sy, -cy, 0.0);
	const DVector3 levelUp = flatRight ^ fwd;
	roll = atan2(right | levelUp, right | flatRight) * R2D;
}
