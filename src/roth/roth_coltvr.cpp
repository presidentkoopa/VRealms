//
// roth_coltvr.cpp -- the Colt as a real M1911 in VR, in the engine. roth_coltvr.h says what each part does; the rules
// are roth_colt.h (engine-free, tools/colt_test).
//
// THE FIT (owner's plan 2026-11-16; tools/colt_test/colt_fit.cpp, its output tools/colt_test/colt_fit.txt): the hand
// stays the authored grip_pistol; the gun is placed in the hand frame by three measured points -- the beavertail on the
// thumb-index web, the index pad toward the trigger face, the grip in the wrapped fingers (no finger in the gun) -- by
// least squares; the index on the trigger is the in-between of the authored open frame and the grip's own index whose
// pad meets the face, and the thumb, middle, ring and little each closed from the open frame until they meet the gun.
// On the Godot XR Tools hands (2026-11-16) every colt_fit check passes (right / left; colt_fit.txt): the pad 2.5 / 2.3 mm
// from the trigger face, the web 4.2 / 7.1 mm from the beavertail, no finger skin more than 2 mm in the gun, the middle,
// ring and little fingers' end segments on the grip (0.0 / 0.0 / 1.9 and 0.1 / 0.3 / 0.1 mm).
// `roth_colt fit` nudges it live (never archived); the owner's result is baked in here.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_coltvr.h"
#include "roth_vrpick.h"
#include "roth_hands.h"
#include "roth_interface.h"
#include "roth_inventory.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "info.h"
#include "r_data/models.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"
#include "r_data/vr_posemath.h"
#include "r_data/model_reach.h"
#include "model.h"
#include "p_maputl.h"
#include "c_cvars.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The engine's own support-hold mode (hw_vrmodes.cpp), read only: R9 takes it as the Shotgun's fore-end does. File scope.
EXTERN_CVAR(Int, vr_support_mode)

namespace roth
{

namespace
{

using namespace colt;

// ---- the fit (colt_fit.txt, 2026-11-16, on the Godot XR Tools hands: vrealms_iwad/models/hands/godot_xr) ----
// The seat: the gun's render coords (file x, z, y) -> the hand frame (X fingers, Y back of the hand, Z thumb axis) in
// the gun's file units, column-major; MODELDEF Scale (metres a unit) then takes it to metres (models.cpp, the grip path).
constexpr float kSeat[2][16] =
{
	{ 0.0892910168f, 0.995177693f, 0.0406014064f, 0.f, 0.0503695027f, 0.0362002479f, -0.998074374f, 0.f,
	  0.994731133f, -0.0911641483f, 0.0468942485f, 0.f, 256.917832f, 86.2404987f, -109.790016f, 1.f },     // right (Hand_Low)
	{ -0.0706316313f, -0.996925039f, 0.0339358085f, 0.f, 0.0669132504f, 0.0292088636f, 0.997331168f, 0.f,
	  0.99525564f, -0.0727138826f, -0.0646444251f, 0.f, 255.514448f, 87.3340048f, 112.069804f, 1.f },     // left (Hand_Low1)
};
// The fingers measured on the gun (vr_handpose_math.h HeldState; each the in-between of the authored open frame and
// grip_pistol's own finger, the knuckle by its own amount): right, left.
constexpr float kIndexReady[2] = { 0.1682f, 0.2003f };   // the index's outer joints with its pad on the trigger face
constexpr float kIndexFire[2] = { 0.3282f, 0.3553f };    // ... and with the trigger back its travel (10 units)
constexpr float kIndexBase[2] = { 1.0f, 1.0f };          // the index's knuckle, the same at trigger 0 and 1
// thumb, index (unused: kIndex*), middle, ring, little: the outer joints, then the knuckles
constexpr float kWrap[2][5] = { { 0.4f, -1.f, 0.8902f, 0.6629f, 0.4448f }, { 0.2f, -1.f, 0.9429f, 0.6261f, 0.5952f } };
constexpr float kWrapBase[2][5] = { { 0.0f, -1.f, 0.7753f, 0.9140f, 0.6449f }, { 0.0f, -1.f, 0.7743f, 1.0f, 0.7344f } };
constexpr double kUnit = 0.000271900171;                 // metres a gun file unit (0.216 m muzzle to tang)

// ---- R9, two hands (tools/colt_test/support_fit.txt, 2026-11-23): the support hand's frame in the SHOOTING hand's frame
// (metres, column-major; the hand frames of vr_handpose_math.h), and its fingers (thumb, index, middle, ring, little:
// knuckle / outer, open -> fbx_twohand_support). [0]: the right hand shoots, the left supports; [1]: the mirror.
// The fit: no skin of either hand more than 2 mm into the gun or the other hand; the thumb forward on the frame under the
// shooting thumb; the index, middle and ring ends on the shooting fingers in front of the strap. NOT met: the support
// palm's centre stands 25 / 33 mm out from the off panel (the shooting fingertips come round onto it).
constexpr double kSupportSeat[2][16] =
{
	{ 0.957706181, 0.286287182, -0.0289572084, 0, 0.282343079, -0.954360022, -0.0973618765, 0,
	  -0.0555090592, 0.0850682036, -0.994827697, 0, 0.017301433, 0.00339735343, 0.0314832141, 1 },
	{ 0.910755632, 0.382023523, -0.156787138, 0, 0.403268705, -0.90452604, 0.138589303, 0,
	  -0.0888736754, -0.189448334, -0.977860316, 0, 0.00696863994, -0.00629817851, -0.0277350184, 1 },
};
constexpr float kSupportWrap[2][5] = { { 1.f, 1.f, 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f, 1.f, 1.f } };       // outer joints
constexpr float kSupportWrapBase[2][5] = { { 1.f, 1.f, 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f, 1.f, 1.f } };   // knuckles

// Points of the gun, render coords (file x, z, y), file units (colt_fit's measures; roth_colt.h's take frames).
const DVector3 kMuzzle(-0.066, 196.51, 395.90);
const DVector3 kBoreRear(-0.066, 196.51, 0.0);
const DVector3 kCaseAt(-0.07, 194.46, -25.51);            // the chambered round's case (Gils), its box centre
const DVector3 kMagAt(0.0, -43.71, -219.59);              // the magazine (Magaz), its box centre
const DVector3 kMagButton(-36.8, 18.4, -126.3);           // the release, the frame's left side behind the guard
const DVector3 kSerrations(0.0, 215.0, -260.0);           // the slide's rear serrations, on top
const DVector3 kBeaver(0.0, 83.14, -344.92);              // the tuning's pivot: the web under the tang
const DVector3 kEject(0.8, 0.55, -0.2);                   // out of the port: right, up, a little back (render axes)
constexpr double kTravel = 180.0 * kUnit;                 // the slide's travel (take frame 119)

// The belt pouch: off-hand side, front of the belt (the body frame of roth_vrweapons: metres from the eye, forward /
// toward the off side / up), and its radius.
constexpr double kPouchF = 0.14, kPouchSide = 0.12, kPouchU = -0.62, kPouchR = 0.11;
constexpr double kAtButton = 0.035, kAtSlide = 0.07, kAtWell = 0.045;   // reach, metres

struct State
{
	FLevelLocals *level = nullptr;
	Rig rig;
	Hands hands;
	AActor *gun = nullptr;          // the drawn gun this tic (PresentHeld's actor), null when not drawn
	AActor *heldMag = nullptr;      // the magazine in the off hand
	bool reloadDue = false;
	bool spawnCase = false, spawnMag = false;
	bool cmdRelease = false, cmdSlide = false;
	bool trigWas = false;
	DVector3 rackOff, rackGun;      // where the off hand and the serrations were when the slide was taken
	int dryLogTic = -1000;
	double tune[6] = { 0, 0, 0, 0, 0, 0 };   // mm and degrees in the hand frame (roth_colt fit)
	Act lastAct = ACT_NONE;
	// R9: the off hand on the grip
	SupportGrip support;
	SupportPlace place;          // this tic's tracked off hand against the fitted place
	bool heldBySet = false;      // the off hand drawn on the gun (VRHandPose_SetHeldBy)
	bool rawTrigWas = false, buttonWas = false;
	int releaseRole = VR_OFFHAND;    // the hand whose press asked for the magazine (a refusal buzzes it)
	int pulse2Tic = -1, pulse2Role = 0; double pulse2Amp = 0; int pulse2Tics = 0;   // a refusal's second buzz
} S;

AActor *Pawn()
{
	if (consoleplayer < 0 || consoleplayer >= MAXPLAYERS) return nullptr;
	return players[consoleplayer].mo;
}

bool HandsOn(AActor *pmo) { return pmo != nullptr && pmo->OverrideAttackPosDir && VRHandPose_InputLive(); }
double UPM() { return VRPickUnitsPerMetre(); }
double PS() { return S.level != nullptr && S.level->pixelstretch > 0.1 ? S.level->pixelstretch : 1.2; }
double Metres(const DVector3 &d) { const double u = UPM(); return sqrt((d.X / u) * (d.X / u) + (d.Y / u) * (d.Y / u) + (d.Z * PS() / u) * (d.Z * PS() / u)); }

bool Alive(AActor *a) { return a != nullptr && !(a->ObjectFlags & OF_EuthanizeMe); }

bool GunPoint(const DVector3 &render, DVector3 &world)
{
	return Alive(S.gun) && Model_ActorPointToWorld(S.gun, render, world);
}

// THE COLT'S ONE HAPTIC OUTPUT: VR_ScriptHaptic, the engine's live haptic call (OpenXR's xrApplyHapticFeedback through
// VRMode::Vibrate) -- the same output L2's feedback uses (roth_vrpick.cpp Haptic). With no headset (the controller-pose
// test) nothing is sent and a line says what would have been.
void Pulse(int role, double amp, int tics, const char *why)
{
	const VRMode *vm = VRMode::GetVRModeCached(true);
	const bool real = !VRPoseTest_Active() && vm != nullptr && vm->IsVR();
	if (real) VR_ScriptHaptic(role, amp, tics * 1000.0 / TICRATE);
	else Printf("[roth_colt] %s hand haptic %.2f for %d tics (%s; no headset: not sent)\n", role == VR_MAINHAND ? "main" : "off", amp, tics, why);
}

// A refusal: two buzzes, the second 4 tics after the first ends (docs/VR_INTERACTION.md 4b, as RSQ Tactical Rig's).
void DoubleBuzz(int role, const char *why)
{
	Pulse(role, 0.3, 3, why);
	S.pulse2Tic = S.level != nullptr ? S.level->maptime + 3 + 4 : -1;
	S.pulse2Role = role; S.pulse2Amp = 0.3; S.pulse2Tics = 3;
}

// The column-major 4x4 inverse and product, in doubles. GENERAL (Gauss-Jordan): the hand frames in render space are not
// orthonormal (PC run zzzzj06: a transpose-inverse put an exactly placed off hand 93.7 deg from the seat).
void Inv4(const double m[16], double o[16])
{
	double a[4][8];
	for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { a[r][c] = m[c * 4 + r]; a[r][4 + c] = r == c ? 1.0 : 0.0; }
	for (int c = 0; c < 4; c++)
	{
		int piv = c;
		for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[piv][c])) piv = r;
		if (piv != c) for (int k = 0; k < 8; k++) { const double t = a[c][k]; a[c][k] = a[piv][k]; a[piv][k] = t; }
		const double d = a[c][c];
		if (fabs(d) < 1e-15) { for (int i = 0; i < 16; i++) o[i] = (i % 5 == 0) ? 1.0 : 0.0; return; }
		for (int k = 0; k < 8; k++) a[c][k] /= d;
		for (int r = 0; r < 4; r++) if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; }
	}
	for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[c * 4 + r] = a[r][4 + c];
}
void Mul4d(const double a[16], const double b[16], double o[16])
{
	double r[16];
	for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { double t = 0; for (int k = 0; k < 4; k++) t += a[k * 4 + rr] * b[c * 4 + k]; r[c * 4 + rr] = t; }
	memcpy(o, r, sizeof(r));
}
bool FrameOf(int role, double out[16])
{
	VSMatrix m;
	if (!VRHandPose_TrackedHandFrame(role, m)) return false;
	const FLOATTYPE *g = m.get();
	for (int i = 0; i < 16; i++) out[i] = g[i];
	return true;
}
int SupportSide() { return VRHandPose_SideForRole(VR_MAINHAND) == VRHandPose::SIDE_LEFT ? 1 : 0; }

// R9: the off hand drawn on the gun while it supports, its tracked place no longer moving it; or not.
void SetSupportDrawn(bool on)
{
	if (on == S.heldBySet) return;
	if (on)
	{
		float rel[16];
		for (int i = 0; i < 16; i++) rel[i] = (float)kSupportSeat[SupportSide()][i];
		VRHandPose_SetHeldBy(VR_OFFHAND, rel);
	}
	else VRHandPose_SetHeldBy(VR_OFFHAND, nullptr);
	S.heldBySet = on;
}

// A map point measured on the DRAWN off hand (the picker's fingertip) where it would be on the TRACKED hand: the same
// coordinates in the hand's own map frame (origin, fwd, right, up), the frame swapped. Equal while the hand is not drawn
// elsewhere.
bool TrackedPoint(const DVector3 &drawn, DVector3 &tracked)
{
	DVector3 o, f, r, u, to, tf, tr, tu;
	if (!VRHandPose_MapFrame(VR_OFFHAND, o, f, r, u) || !VRHandPose_TrackedMapFrame(VR_OFFHAND, to, tf, tr, tu)) return false;
	const DVector3 d = drawn - o;
	auto det3 = [](const DVector3 &a, const DVector3 &b, const DVector3 &c) { return a | (b ^ c); };
	const double D = det3(f, r, u);
	if (fabs(D) < 1e-12) return false;
	const double a = det3(d, r, u) / D, b = det3(f, d, u) / D, c = det3(f, r, d) / D;
	tracked = to + tf * a + tr * b + tu * c;
	return true;
}

// column-major 4x4
void Mul4(const float a[16], const float b[16], float o[16])
{
	float r[16];
	for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k]; r[c * 4 + rr] = s; }
	memcpy(o, r, sizeof(r));
}
void Ident(float m[16]) { memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1.f; }
void Trans(float m[16], double x, double y, double z) { Ident(m); m[12] = (float)x; m[13] = (float)y; m[14] = (float)z; }
void RotAxis(float m[16], int axis, double deg)
{
	Ident(m);
	const float c = (float)cos(deg * M_PI / 180), s = (float)sin(deg * M_PI / 180);
	const int a = (axis + 1) % 3, b = (axis + 2) % 3;
	m[a * 4 + a] = c; m[a * 4 + b] = s; m[b * 4 + a] = -s; m[b * 4 + b] = c;
}

// The seat with the live tuning: rotate about the beavertail (in the hand frame), then move; in gun units.
void SeatFor(int side, float out[16])
{
	const float *base = kSeat[side == VRHandPose::SIDE_LEFT ? 1 : 0];
	const float pv[3] = { base[0] * (float)kBeaver.X + base[4] * (float)kBeaver.Y + base[8] * (float)kBeaver.Z + base[12],
		base[1] * (float)kBeaver.X + base[5] * (float)kBeaver.Y + base[9] * (float)kBeaver.Z + base[13],
		base[2] * (float)kBeaver.X + base[6] * (float)kBeaver.Y + base[10] * (float)kBeaver.Z + base[14] };
	float t1[16], rx[16], ry[16], rz[16], t2[16], m[16];
	const double mm = 0.001 / kUnit;
	Trans(t2, pv[0] + S.tune[0] * mm, pv[1] + S.tune[1] * mm, pv[2] + S.tune[2] * mm);
	RotAxis(rx, 0, S.tune[3]); RotAxis(ry, 1, S.tune[4]); RotAxis(rz, 2, S.tune[5]);
	Trans(t1, -pv[0], -pv[1], -pv[2]);
	Mul4(t2, rz, m); Mul4(m, ry, m); Mul4(m, rx, m); Mul4(m, t1, m); Mul4(m, base, out);
}

// The part of the take each joint shows, sampled between two frames.
void PoseJoint(AActor *a, FModel *m, const char *name, double frame, bool hidden)
{
	const FName jn(name);
	const int j = m->FindJoint(jn);
	if (j < 0) return;
	const int n = m->NumFrames();
	if (n <= 0) return;
	if (frame < 0) frame = 0;
	if (frame > n - 1) frame = n - 1;
	const int f0 = (int)floor(frame), f1 = f0 + 1 < n ? f0 + 1 : f0;
	const float t = (float)(frame - f0);
	const TRS a0 = m->GetJointPose(j, f0), a1 = m->GetJointPose(j, f1);
	const FVector3 tr = a0.translation + (a1.translation - a0.translation) * t;
	const FQuaternion q = FQuaternion::SLerp(a0.rotation, a1.rotation, t);
	ModelDrawPose_SetJoint(a, jn, 0, hidden ? 3 : 2, q, 2, tr);
}

void ApplyParts(AActor *gun, const Parts &p)
{
	FModel *m = Model_ActorModel(gun);
	if (m == nullptr) return;
	PoseJoint(gun, m, "Slide", p.slide, false);
	PoseJoint(gun, m, "Dyl", p.barrel, false);
	PoseJoint(gun, m, "Hammer", p.hammer, false);
	PoseJoint(gun, m, "Trig", p.trigger, false);
	PoseJoint(gun, m, "Mag_button", p.magButton, false);
	PoseJoint(gun, m, "Magaz", kTakeRest, !p.magShown);
	PoseJoint(gun, m, "Bullet_2", p.topRound, !p.topRoundShown);
	PoseJoint(gun, m, "Bullet", kTakeRest, !p.chamberShown);
	PoseJoint(gun, m, "Gils", kTakeRest, !p.chamberShown);
}

AActor *SpawnPart(const char *cls, const DVector3 &at, const DVector3 &vel, double yaw)
{
	PClassActor *c = PClass::FindActor(cls);
	if (c == nullptr || S.level == nullptr) { Printf(TEXTCOLOR_RED "[roth_colt] no actor class %s\n", cls); return nullptr; }
	AActor *a = AActor::StaticSpawn(S.level, c, at, NO_REPLACE);
	if (a == nullptr) return nullptr;
	const double k = UPM() * kUnit;                 // map units a gun file unit, as roth_img3d sizes a world model
	a->Scale = DVector2(k, k / PS());
	a->PrevScale = a->Scale;
	a->Vel = vel;
	a->Angles.Yaw = DAngle::fromDeg(yaw);
	// 9.8 m/s^2 in map units a tic squared, through the level's own gravity (AActor::GetGravity: level x sector x
	// Gravity / 800)
	const double lg = S.level->gravity > 0 ? S.level->gravity : 800.0;
	a->Gravity = 9.8 * UPM() / ((double)TICRATE * TICRATE) / (lg * 0.00125);
	return a;
}

const char *ActName(Act a)
{
	switch (a)
	{
	case ACT_DROP_MAG: return "MAGAZINE DROPPED";
	case ACT_REFUSE_DROP: return "release refused";
	case ACT_TAKE_MAG: return "MAGAZINE TAKEN";
	case ACT_REFUSE_TAKE: return "take refused";
	case ACT_RETURN_MAG: return "magazine back to the pouch";
	case ACT_SEAT: return "MAGAZINE SEATED";
	case ACT_SEAT_WAIT: return "seated, waiting for the lock";
	case ACT_RACK_BACK: return "SLIDE BACK";
	case ACT_SLIDE_FORWARD: return "SLIDE FORWARD";
	case ACT_DRY: return "dry";
	case ACT_BUTTON_NONE: return "the button: nothing";
	default: return "-";
	}
}

} // namespace (anonymous)

PClassActor *ColtMeshClass()
{
	static PClassActor *cls = nullptr;
	static bool looked = false;
	if (!looked) { cls = PClass::FindActor("RealmsColt1911"); looked = true; }
	return cls;
}

void ColtHandsTick(FLevelLocals *level, bool live)
{
	if (level == nullptr) return;
	if (S.level != level) { const double keep[6] = { S.tune[0], S.tune[1], S.tune[2], S.tune[3], S.tune[4], S.tune[5] }; SetSupportDrawn(false); S = State(); memcpy(S.tune, keep, sizeof(keep)); S.level = level; }
	if (S.pulse2Tic >= 0 && level->maptime >= S.pulse2Tic) { Pulse(S.pulse2Role, S.pulse2Amp, S.pulse2Tics, "refused, the second buzz"); S.pulse2Tic = -1; }
	AActor *pmo = Pawn();
	Hands h;
	h.vr = HandsOn(pmo) && pmo->health > 0;
	h.reloadInput = S.cmdRelease;
	h.slideRelease = S.cmdSlide;
	if (S.cmdRelease) S.releaseRole = VR_MAINHAND;
	S.cmdRelease = S.cmdSlide = false;
	// R8: the shooting hand's lower face button (A / X), read as the trigger and grip are (VRFingerInput::faceLower)
	VRFingerInput mainIn;
	const bool button = VRHandPose_Fingers(VR_MAINHAND, mainIn) && mainIn.faceLower;
	const bool buttonEdge = button && !S.buttonWas;
	S.buttonWas = button;
	if (!h.vr || !live || !Alive(S.gun))
	{
		const bool ri = h.reloadInput, sr = h.slideRelease;
		h = Hands(); h.vr = HandsOn(pmo); h.reloadInput = ri; h.slideRelease = sr;
		S.hands = h;
		if (S.support.engaged) { S.support.Reset(); Printf("[roth_colt] TWO HANDS: the off hand lets go -- the gun is not drawn in the hands\n"); }
		SetSupportDrawn(false);
		return;
	}
	if (buttonEdge && HandMayAct(VR_MAINHAND, hands::Owner::Colt))
	{
		h.mainButton = true;
		S.releaseRole = VR_MAINHAND;
		Printf("[roth_colt] the shooting hand's button (%s) pressed: magazine %s, slide %s\n",
			VRHandPose_SideForRole(VR_MAINHAND) == VRHandPose::SIDE_LEFT ? "X" : "A", S.rig.magIn ? "in" : "out", S.rig.slideLocked ? "locked back" : "forward");
	}

	VRPickHand off;
	const bool haveOff = VRPickHandNow(VR_OFFHAND, off);
	// THE OFF HAND AS TRACKED: while it supports it is DRAWN on the gun (VRHandPose_SetHeldBy), so everything the Colt
	// asks of it -- the catch, the pouch, the slide, the latch itself -- is measured where the player's hand really is.
	DVector3 sock, f, r, u;
	const bool haveSock = VRHandPose_TrackedMapFrame(VR_OFFHAND, sock, f, r, u);
	const bool rawTrig = haveOff && off.trigDown;
	const bool rawTrigEdge = rawTrig && !S.rawTrigWas;
	S.rawTrigWas = rawTrig;
	DVector3 p;
	if (haveOff && off.havePoints && GunPoint(kMagButton, p))
	{
		DVector3 tip = off.tip;
		if (S.heldBySet) TrackedPoint(off.tip, tip);
		h.offAtMagButton = Metres(tip - p) < kAtButton;
	}
	if (haveSock)
	{
		// the pouch, in the body frame (metres from the eye; forward, toward the off side, up)
		const DVector3 head(pmo->X(), pmo->Y(), pmo->player != nullptr ? pmo->player->viewz : pmo->Z());
		const DVector3 d = sock - head;
		const double yaw = pmo->Angles.Yaw.Radians(), upm = UPM();
		const double fw = (d.X * cos(yaw) + d.Y * sin(yaw)) / upm, lf = (-d.X * sin(yaw) + d.Y * cos(yaw)) / upm, up = d.Z * PS() / upm;
		const double side = VRHandPose_SideForRole(VR_OFFHAND) == VRHandPose::SIDE_LEFT ? 1.0 : -1.0;   // + left
		const double dd = sqrt((fw - kPouchF) * (fw - kPouchF) + (lf - side * kPouchSide) * (lf - side * kPouchSide) + (up - kPouchU) * (up - kPouchU));
		h.offAtPouch = dd < kPouchR;
		if (GunPoint(kSerrations, p))
		{
			h.offAtSlide = Metres(sock - p) < kAtSlide;
			DVector3 muz, rear;
			if (S.rig.racking && GunPoint(kMuzzle, muz) && GunPoint(kBoreRear, rear))
			{
				const DVector3 back = (rear - muz).Unit();
				const DVector3 moved = (sock - S.rackOff) - (p - S.rackGun);
				h.slideAlong = Metres(back * (moved | back)) * ((moved | back) >= 0 ? 1 : -1);
			}
			else { S.rackOff = sock; S.rackGun = p; }
		}
		DVector3 well;
		if (Alive(S.heldMag) && GunPoint(kMagAt, well))
		{
			DVector3 heldAt;
			if (Model_ActorPointToWorld(S.heldMag, DVector3(0, 0, 0), heldAt)) h.magAtWell = Metres(heldAt - well) < kAtWell;
		}
	}

	// R9: TWO HANDS. The tracked off hand against the place support_fit measured, in the shooting hand's frame.
	double mainF[16], offF[16];
	if (FrameOf(VR_MAINHAND, mainF) && FrameOf(VR_OFFHAND, offF))
	{
		double inv[16], have[16];
		Inv4(mainF, inv);
		Mul4d(inv, offF, have);
		S.place = SupportPlaceOf(kSupportSeat[SupportSide()], have);
	}
	else S.place = SupportPlace();
	const bool reloadAction = (h.offAtMagButton && rawTrigEdge) || h.offAtPouch || h.offAtSlide;
	const bool allowed = !S.rig.magInHand && !S.rig.racking;
	// L2 (roth_handstate.h): the grip is the weapon's only while nobody else holds that hand (an item, a held object)
	const bool supGrip = haveOff && off.gripClosed && HandMayAct(VR_OFFHAND, hands::Owner::Weapon);
	const int ev = S.support.Feed(S.place, supGrip, vr_support_mode, reloadAction, allowed);
	if (ev != 0)
	{
		Printf("[roth_colt] TWO HANDS: the off hand %s -- %s (%.3f m and %.1f deg from the fitted place)%s\n", ev > 0 ? "TAKES the grip" : "lets go of the grip",
			S.support.why, S.place.dist, S.place.deg, ev > 0 ? "; drawn there now, its tracked place no longer moves it; the aim stays the shooting hand's bore" : "");
		Pulse(VR_OFFHAND, ev > 0 ? 0.4 : 0.25, ev > 0 ? 3 : 2, ev > 0 ? "the off hand takes the grip" : "the off hand lets go of the grip");
	}
	SetSupportDrawn(S.support.engaged);

	// L2 (roth_handstate.h): the off hand acts on the gun while the Colt owns it or nobody does (not while it holds an
	// item or a held object, or supports a weapon). A support let go this tic for a reload action passes that action on.
	const bool mine = HandMayAct(VR_OFFHAND, hands::Owner::Colt) || (ev < 0 && reloadAction);
	h.offGrip = haveOff && off.gripClosed && mine;
	h.magButtonPress = mine && rawTrigEdge;
	if (h.magButtonPress && h.offAtMagButton) S.releaseRole = VR_OFFHAND;
	S.trigWas = rawTrig && mine;
	S.hands = h;
}

bool ColtPullReaches(const Game &g)
{
	if (PullReachesOriginal(S.rig, g, S.hands.vr)) return true;
	const int now = S.level != nullptr ? S.level->maptime : 0;
	if (now - S.dryLogTic > 35)
	{
		S.dryLogTic = now;
		Printf("[roth_colt] the pull is a DRY CLICK (not trigger_weapon_fire): the word %d, i51 carried %d, magazine %s, slide %s, %s\n", g.qty, g.mags,
			S.rig.magIn ? "in" : "out", S.rig.slideLocked ? "locked back" : (S.rig.racking ? "in the off hand" : "forward"),
			g.qty <= 0 ? "the empty word with magazines carried: the reload is the hands' (R5)" : "not chambered (R6)");
		Pulse(VR_MAINHAND, 0.15, 2, "a dry click");
	}
	return false;
}

bool ColtReloadDue(const Game &g)
{
	if (!S.reloadDue) return false;
	S.reloadDue = false;
	if (!g.colt || g.qty != 0 || g.mags <= 0 || g.lock != 0)
	{
		Printf("[roth_colt] the seat's reload NOT pulled: the original would not reload now (word %d, i51 %d, lock %d)\n", g.qty, g.mags, g.lock);
		return false;
	}
	return true;
}

void ColtReloadDone(int qtyBefore, int magsBefore, int qtyAfter, int magsAfter, int lockAfter)
{
	Printf("[roth_colt] the magazine seated: the original's reload pulled (trigger_weapon_fire, WC:843-866): i51 carried %d -> %d, the Colt's"
		" word %d -> %d, the reload picture's lock %d (no shot)%s\n", magsBefore, magsAfter, qtyBefore, qtyAfter, lockAfter,
		(magsAfter == magsBefore - 1 && qtyAfter == qtyBefore + 12) ? " -- one i51 consumed, 12 rounds: as the original" : " -- NOT the original's numbers");
}

void ColtAfterWeapon(const Game &g)
{
	if (!g.colt) { S.rig.gripWas = S.hands.offGrip; return; }
	const Event e = Step(S.rig, g, S.hands, kTravel);
	S.hands.reloadInput = S.hands.slideRelease = S.hands.mainButton = false;
	if (g.shotLeft && S.hands.vr)
	{
		S.spawnCase = true;
		// the recoil: the shooting hand, and the supporting hand with it (R9)
		Pulse(VR_MAINHAND, 0.9, 4, "recoil");
		if (S.support.engaged) Pulse(VR_OFFHAND, 0.6, 4, "recoil, the supporting hand");
	}
	if (g.shotLeft && S.hands.vr && g.qty == 0) Printf("[roth_colt] the last round fired: the slide locks back at the cycle's end (the word 0, i51 carried %d)\n", g.mags);
	if (e.act == ACT_NONE) return;
	S.lastAct = e.act;
	Printf("[roth_colt] %s -- %s (the word %d, i51 carried %d, lock %d)\n", ActName(e.act), e.why, g.qty, g.mags, g.lock);
	switch (e.act)
	{
	case ACT_DROP_MAG: S.spawnMag = true; Pulse(VR_MAINHAND, 0.3, 3, "magazine out"); break;
	case ACT_REFUSE_DROP: DoubleBuzz(S.releaseRole, "the release refused: the magazine does not move (R1)"); break;
	case ACT_REFUSE_TAKE: DoubleBuzz(VR_OFFHAND, "no magazine to take (R2)"); break;
	case ACT_TAKE_MAG: Pulse(VR_OFFHAND, 0.4, 3, "magazine taken"); break;
	case ACT_SEAT: if (g.qty == 0) S.reloadDue = true; Pulse(VR_MAINHAND, 0.6, 4, "the magazine seats"); Pulse(VR_OFFHAND, 0.6, 4, "the magazine seats"); break;
	case ACT_RACK_BACK: Pulse(VR_OFFHAND, 0.5, 3, "slide back"); break;
	case ACT_SLIDE_FORWARD: Pulse(VR_MAINHAND, 0.5, 3, "the slide closes"); break;
	default: break;
	}
}

void ColtPresent(AActor *gun, const Game &g)
{
	S.gun = gun;
	if (gun == nullptr) return;
	const int side = VRHandPose_SideForRole(VR_MAINHAND);
	gun->FollowHandMode = FOLLOWHAND_GRIP_MAIN;
	DVector3 o, f, r, u;
	if (VRHandPose_MapFrame(VR_MAINHAND, o, f, r, u)) gun->SetOrigin(o, true);
	const AActor *def = GetDefaultByType(gun->GetClass());
	if (def != nullptr && def->SpawnState != nullptr) { gun->sprite = def->SpawnState->sprite; gun->frame = 0; }
	float seat[16];
	SeatFor(side, seat);
	VRHandPose_SetActorSeat(gun, seat);
	ApplyParts(gun, PartsFor(S.rig, g));

	// the spent case, out of the port with the shot
	if (S.spawnCase)
	{
		S.spawnCase = false;
		DVector3 at, out;
		if (GunPoint(kCaseAt, at) && GunPoint(kCaseAt + kEject * 100.0, out))
		{
			const DVector3 dir = (out - at).Unit();
			const double v = UPM() * 2.2 / TICRATE;          // 2.2 m/s
			AActor *c = SpawnPart("RealmsColtCasing", at, dir * v, (out - at).Angle().Degrees());
			if (c != nullptr) Printf("[roth_colt] the spent case thrown from the port at (%.1f %.1f %.1f), %.2f m/s along (%.2f %.2f %.2f)\n", at.X, at.Y, at.Z, 2.2, dir.X, dir.Y, dir.Z);
		}
	}
	// the dropped magazine, out of the grip
	if (S.spawnMag)
	{
		S.spawnMag = false;
		DVector3 at, below;
		if (GunPoint(kMagAt, at) && GunPoint(kMagAt + DVector3(0, -100, 0), below))
		{
			const DVector3 dir = (below - at).Unit();
			AActor *m = SpawnPart("RealmsColtMagazine", at, dir * (UPM() * 0.6 / TICRATE), (S.gun->Angles.Yaw).Degrees());
			if (m != nullptr) Printf("[roth_colt] the empty magazine falls free of the grip at (%.1f %.1f %.1f)\n", at.X, at.Y, at.Z);
		}
	}
	// the magazine in the off hand
	if (S.rig.magInHand)
	{
		if (!Alive(S.heldMag))
		{
			PClassActor *c = PClass::FindActor("RealmsColtMagazineHeld");
			DVector3 so, sf, sr, su;
			if (c != nullptr && VRHandPose_MapFrame(VR_OFFHAND, so, sf, sr, su)) S.heldMag = AActor::StaticSpawn(S.level, c, so, NO_REPLACE);
		}
		if (Alive(S.heldMag))
		{
			DVector3 so, sf, sr, su;
			if (VRHandPose_MapFrame(VR_OFFHAND, so, sf, sr, su)) S.heldMag->SetOrigin(so, true);
			S.heldMag->FollowHandMode = FOLLOWHAND_GRIP_OFF;
			// held as a gun's grip is: its long axis through the off hand's fingers, its feed lips toward the thumb, its
			// front toward the fingertips -- the off side's thumb axis (vr_handpose_math.h: thumb -Z right, +Z left)
			const bool left = VRHandPose_SideForRole(VR_OFFHAND) == VRHandPose::SIDE_LEFT;
			const float up = left ? 1.f : -1.f;
			// render (x, y=file z, z=file y): file x -> the back of the hand (Y) * side, file z (up the magazine) -> thumb,
			// file y (its front) -> fingers (X)
			const float mseat[16] = { 0.f, -up, 0.f, 0.f,   0.f, 0.f, up, 0.f,   1.f, 0.f, 0.f, 0.f,   0.f, 0.f, 0.f, 1.f };
			VRHandPose_SetActorSeat(S.heldMag, mseat);
		}
	}
	else if (Alive(S.heldMag)) { VRHandPose_SetActorSeat(S.heldMag, nullptr); S.heldMag->Destroy(); S.heldMag = nullptr; }
}

bool ColtBoreAim(double &yawDeg, double &pitchUpDeg, DVector3 &muzzle)
{
	if (!Alive(S.gun) || !S.hands.vr) return false;
	DVector3 rear;
	if (!GunPoint(kMuzzle, muzzle) || !GunPoint(kBoreRear, rear)) return false;
	const DVector3 d = muzzle - rear;
	const double run = sqrt(d.X * d.X + d.Y * d.Y);
	if (d.Length() < 1e-6) return false;
	yawDeg = atan2(d.Y, d.X) * (180.0 / M_PI);
	pitchUpDeg = atan2(d.Z, run) * (180.0 / M_PI);
	return true;
}

bool ColtHeldFingers(int side, VRHandPose::HeldState &held)
{
	if (!Alive(S.gun) || !S.hands.vr) return false;
	const int k = side == VRHandPose::SIDE_LEFT ? 1 : 0;
	held.indexReady = kIndexReady[k];
	held.indexFire = kIndexFire[k];
	held.indexBase = kIndexBase[k];
	for (int f = 0; f < 5; f++)
	{
		held.wrap[f] = kWrap[k][f];
		held.wrapBase[f] = kWrapBase[k][f];
	}
	return true;
}

bool ColtGripTaken(int role)
{
	if (role != VR_OFFHAND || !S.hands.vr || !Alive(S.gun)) return false;
	// (the catch only while not supporting: R9's own reload action lets the support go first, the next tic the catch's)
	return S.rig.magInHand || S.rig.racking || S.hands.offAtPouch || S.hands.offAtSlide || (S.hands.offAtMagButton && !S.support.engaged);
}

bool ColtSupportHeld()
{
	return S.support.engaged && S.hands.vr && Alive(S.gun);
}

bool ColtSupportWouldTake()
{
	if (!S.hands.vr || !Alive(S.gun) || S.rig.magInHand || S.rig.racking || S.support.rearmRequired || vr_support_mode == 0) return false;
	return S.support.engaged || (S.place.dist <= kSupportEnterM && S.place.deg <= kSupportEnterDeg);
}

bool ColtSupportFingers(VRHandPose::HeldState &held)
{
	if (!ColtSupportHeld()) return false;
	const int k = SupportSide();
	held = VRHandPose::HeldState();
	held.subject = VRHandPose::GRIPSUBJ_Support;   // hold_support: the owner's fbx_twohand_support (SelectHold, VAP :587-654)
	held.item = kColt;
	for (int f = 0; f < 5; f++) { held.wrap[f] = kSupportWrap[k][f]; held.wrapBase[f] = kSupportWrapBase[k][f]; }
	return true;
}

bool ColtTakesUseKey()
{
	if (!S.hands.vr || !Alive(S.gun)) return false;
	VRFingerInput in;
	return VRHandPose_Fingers(VR_MAINHAND, in) && in.faceLower;
}

void ColtEndLevel()
{
	SetSupportDrawn(false);
	S.support.Reset();
	if (S.gun != nullptr) VRHandPose_SetActorSeat(S.gun, nullptr);
	if (S.heldMag != nullptr) VRHandPose_SetActorSeat(S.heldMag, nullptr);
	S.gun = S.heldMag = nullptr;
	S.level = nullptr;
}

// The drawn gun goes away (put away, the card, another weapon): its seat with it.
void ColtGunGone()
{
	if (S.gun != nullptr) { VRHandPose_SetActorSeat(S.gun, nullptr); ModelDrawPose_ClearActor(S.gun); }
	S.gun = nullptr;
	if (S.support.engaged) Printf("[roth_colt] TWO HANDS: the off hand lets go -- the gun went away\n");
	S.support.Reset();
	SetSupportDrawn(false);
}

} // namespace roth

//===========================================================================
//
// roth_colt -- the Colt's real parts. Never archived.
//
//   roth_colt status
//   roth_colt release                 the reload input: the magazine release (bind a key or a controller button to it)
//   roth_colt sliderelease            the slide release (the slide stop)
//   roth_colt fit [dx dy dz rx ry rz | reset]
//                                     nudge the gun in the main hand: millimetres along the hand frame's X (fingers),
//                                     Y (back of the hand), Z (thumb axis), degrees about those axes, about the
//                                     beavertail; prints the seat to bake in (roth_coltvr.cpp kSeat)
//
//===========================================================================

CCMD(roth_colt)
{
	using namespace roth;
	if (argv.argc() < 2 || !stricmp(argv[1], "status"))
	{
		const Rig &r = S.rig;
		Printf("roth_colt: hands %s, gun %s; magazine %s%s, slide %s%s, chambered %d; tune %.1f %.1f %.1f mm %.1f %.1f %.1f deg; last %s\n",
			S.hands.vr ? "live" : "off", Alive(S.gun) ? "drawn" : "not drawn", r.magIn ? "in" : "out", r.magInHand ? " (one in the off hand)" : "",
			r.slideLocked ? "locked back" : "forward", r.racking ? " (racking)" : "", (int)r.chambered,
			S.tune[0], S.tune[1], S.tune[2], S.tune[3], S.tune[4], S.tune[5], ActName(S.lastAct));
		Printf("roth_colt: off hand at the release %d, at the pouch %d, at the slide %d, magazine at the well %d, grip %d\n",
			(int)S.hands.offAtMagButton, (int)S.hands.offAtPouch, (int)S.hands.offAtSlide, (int)S.hands.magAtWell, (int)S.hands.offGrip);
		Printf("roth_colt: TWO HANDS %s; the tracked off hand %.3f m and %.1f deg from the fitted place (takes within %.2f m / %.0f deg, keeps within %.2f m / %.0f deg)%s\n",
			S.support.engaged ? "SUPPORTING (the off hand drawn on the grip)" : "no", S.place.dist, S.place.deg, kSupportEnterM, kSupportEnterDeg, kSupportExitM, kSupportExitDeg,
			S.support.rearmRequired ? "; after a reload action: the hand must leave first" : "");
		// the loose parts in the world: where each is, how high above its floor, landed or flying
		if (S.level != nullptr)
		{
			PClassActor *pc = PClass::FindActor("RealmsColtPart");
			auto it = S.level->GetThinkerIterator<AActor>();
			AActor *a;
			int n = 0;
			while (pc != nullptr && (a = it.Next()) != nullptr)
			{
				if (!a->IsKindOf(pc)) continue;
				n++;
				Printf("roth_colt: %s at (%.1f %.1f %.1f), %.2f above its floor (%.1f), %s, %d tics left once landed\n", a->GetClass()->TypeName.GetChars(),
					a->X(), a->Y(), a->Z(), a->Z() - a->floorz, a->floorz, a->BoolVar(FName("Landed")) ? "LANDED" : "flying", a->IntVar(FName("RealmsLife")));
			}
			Printf("roth_colt: %d loose part(s) in the world\n", n);
		}
		return;
	}
	if (!stricmp(argv[1], "release")) { S.cmdRelease = true; Printf("roth_colt: the magazine release pressed (the reload input)\n"); return; }
	if (!stricmp(argv[1], "sliderelease")) { S.cmdSlide = true; Printf("roth_colt: the slide release pressed\n"); return; }
	if (!stricmp(argv[1], "fit"))
	{
		if (argv.argc() >= 3 && !stricmp(argv[2], "reset")) for (double &t : S.tune) t = 0;
		else if (argv.argc() >= 8) for (int i = 0; i < 6; i++) S.tune[i] = atof(argv[2 + i]);
		float seat[16];
		for (int side = 0; side < 2; side++)
		{
			SeatFor(side, seat);
			Printf("roth_colt fit: %s hand: %.1f %.1f %.1f mm, %.1f %.1f %.1f deg -> seat { %.9g, %.9g, %.9g, 0, %.9g, %.9g, %.9g, 0, %.9g, %.9g, %.9g, 0, %.9g, %.9g, %.9g, 1 }\n",
				side == 0 ? "right" : "left", S.tune[0], S.tune[1], S.tune[2], S.tune[3], S.tune[4], S.tune[5],
				seat[0], seat[1], seat[2], seat[4], seat[5], seat[6], seat[8], seat[9], seat[10], seat[12], seat[13], seat[14]);
		}
		return;
	}
	Printf("roth_colt status | release | sliderelease | fit [dx dy dz rx ry rz | reset]\n");
}

//===========================================================================
//
// roth_colttest -- test-only, inert until typed; with roth_xrpose (the controller-pose test) driving the hands.
//
//   roth_colttest offhand <magbutton|pouch|well|slide|support> [dx dy dz]
//                     move the off hand (its orientation kept) so that its index tip (magbutton) or its grip socket
//                     (the rest) lands on that point of the drawn gun (the pouch: of the body), plus map units
//   roth_colttest rack <metres>       move the off hand back along the drawn bore by that much (+ back, - forward)
//   roth_colttest lookat <class> <file> [distance]   rothdiff_shot of the first landed part of the class, from that far
//   roth_colttest off <grip|trigger> <0|1>
//
//===========================================================================

namespace roth { namespace {
// The off hand's tracking position solved (affine, exact: vr_posemath.h) so that `probe` lands on `want`.
bool MoveOff(const DVector3 &want, bool tip, const char *what)
{
	AActor *pmo = Pawn();
	FVRPoseTestHand h;
	if (pmo == nullptr || !VRPoseTest_Hand(VR_OFFHAND, h) || !h.valid) { Printf("roth_colttest: the off hand has no pose (roth_xrpose hand off ... first)\n"); return false; }
	auto probe = [&](const float p[3], DVector3 &out) -> bool
	{
		FVRPoseTestHand t = h;
		t.pos[0] = p[0]; t.pos[1] = p[1]; t.pos[2] = p[2];
		VRPoseTest_SetHand(VR_OFFHAND, t);
		VRPoseTest_PublishAim(pmo);
		if (tip) { VRPickHand o; if (!VRPickHandNow(VR_OFFHAND, o) || !o.havePoints) return false; out = o.tip; return true; }
		DVector3 f, r, u;
		return VRHandPose_TrackedMapFrame(VR_OFFHAND, out, f, r, u);
	};
	const float p0[3] = { h.pos[0], h.pos[1], h.pos[2] };
	DVector3 b, col[3];
	if (!probe(p0, b)) { Printf("roth_colttest: the off hand's %s cannot be read\n", tip ? "fingertip" : "grip socket"); return false; }
	for (int i = 0; i < 3; i++)
	{
		float pi[3] = { p0[0], p0[1], p0[2] };
		pi[i] += 0.1f;
		DVector3 q;
		if (!probe(pi, q)) return false;
		col[i] = (q - b) * 10.0;
	}
	const DVector3 r = want - b;
	auto det3 = [](const DVector3 &a, const DVector3 &c1, const DVector3 &c2) { return a | (c1 ^ c2); };
	const double D = det3(col[0], col[1], col[2]);
	if (fabs(D) < 1e-9) { probe(p0, b); Printf("roth_colttest: the device's map is singular\n"); return false; }
	const float pf[3] = { (float)(p0[0] + det3(r, col[1], col[2]) / D), (float)(p0[1] + det3(col[0], r, col[2]) / D), (float)(p0[2] + det3(col[0], col[1], r) / D) };
	DVector3 got;
	probe(pf, got);
	Printf("roth_colttest: the off hand's %s on %s: at (%.1f %.1f %.1f), wanted (%.1f %.1f %.1f), %.2f units off\n", tip ? "index tip" : "grip socket", what,
		got.X, got.Y, got.Z, want.X, want.Y, want.Z, (got - want).Length());
	return true;
}
// THE OFF HAND TURNED so its TRACKED hand frame has the support seat's orientation on the main hand (R9's test): the
// device's own path from the test's tracking-space quaternion to the hand frame is probed, not inverted by hand -- small
// turns about the tracking axes, their effect on the frame measured, a Gauss-Newton step on the rotation's error, eight
// times. Returns the error left, degrees (-1: no pose).
void QMul(const float a[4], const float b[4], float o[4])
{
	const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
	o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
void QFromVec(const double v[3], float q[4])
{
	const double th = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	const double k = th > 1e-12 ? sin(th * 0.5) / th : 0.5;
	q[0] = (float)(v[0] * k); q[1] = (float)(v[1] * k); q[2] = (float)(v[2] * k); q[3] = (float)cos(th * 0.5);
}
// the rotation vector of a * b^T (the 3x3 parts of two column-major frames, orthonormal, of the same handedness)
void RotErr(const double a[16], const double b[16], double v[3])
{
	double M[3][3];
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { double t = 0; for (int k = 0; k < 3; k++) t += a[k * 4 + i] * b[k * 4 + j]; M[i][j] = t; }
	double c = (M[0][0] + M[1][1] + M[2][2] - 1) * 0.5; c = c > 1 ? 1 : (c < -1 ? -1 : c);
	const double th = acos(c);
	const double sn = sin(th);
	if (th < 1e-9) { v[0] = v[1] = v[2] = 0; return; }
	const double k = sn > 1e-6 ? th / (2 * sn) : 0.5;
	v[0] = (M[2][1] - M[1][2]) * k; v[1] = (M[0][2] - M[2][0]) * k; v[2] = (M[1][0] - M[0][1]) * k;
}
double TurnOffToSeat(AActor *pmo)
{
	FVRPoseTestHand h;
	if (!VRPoseTest_Hand(VR_OFFHAND, h) || !h.valid) return -1;
	double mainF[16], want[16], cur[16];
	if (!FrameOf(VR_MAINHAND, mainF)) return -1;
	Mul4d(mainF, kSupportSeat[SupportSide()], want);
	auto frameFor = [&](const float q[4], double out[16]) {
		FVRPoseTestHand t = h;
		for (int k = 0; k < 4; k++) t.quat[k] = q[k];
		VRPoseTest_SetHand(VR_OFFHAND, t);
		VRPoseTest_PublishAim(pmo);
		return FrameOf(VR_OFFHAND, out);
	};
	float q[4] = { h.quat[0], h.quat[1], h.quat[2], h.quat[3] };
	double e[3] = { 0, 0, 0 };
	auto det = [](const double m[3][3]) { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); };
	for (int it = 0; it < 8; it++)
	{
		if (!frameFor(q, cur)) return -1;
		RotErr(want, cur, e);
		double J[3][3];
		for (int i = 0; i < 3; i++)
		{
			const double dv[3] = { i == 0 ? 0.01 : 0.0, i == 1 ? 0.01 : 0.0, i == 2 ? 0.01 : 0.0 };
			float dq[4], qi[4]; QFromVec(dv, dq); QMul(q, dq, qi);
			double ci[16], d[3];
			if (!frameFor(qi, ci)) return -1;
			RotErr(ci, cur, d);
			for (int r = 0; r < 3; r++) J[r][i] = d[r] / 0.01;
		}
		const double D = det(J);
		if (fabs(D) < 1e-12) break;
		double x[3];
		for (int c = 0; c < 3; c++) { double Mc[3][3]; memcpy(Mc, J, sizeof(Mc)); for (int r = 0; r < 3; r++) Mc[r][c] = e[r]; x[c] = det(Mc) / D; }
		float dq[4], qn[4]; QFromVec(x, dq); QMul(q, dq, qn);
		memcpy(q, qn, sizeof(q));
	}
	frameFor(q, cur);
	RotErr(want, cur, e);
	return sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) * 180.0 / M_PI;
}
} }

CCMD(roth_colttest)
{
	using namespace roth;
	AActor *pmo = Pawn();
	if (argv.argc() < 3 || pmo == nullptr) { Printf("roth_colttest hold <x y z yaw> [pitch] | offhand <magbutton|pouch|well|slide> [dx dy dz] | rack <metres> | off <grip|trigger> <0|1>\n"); return; }
	if (!stricmp(argv[1], "offhand"))
	{
		DVector3 want;
		bool tip = false;
		const char *what = argv[2];
		if (!stricmp(what, "magbutton")) { if (!GunPoint(kMagButton, want)) { Printf("roth_colttest: no gun drawn\n"); return; } tip = true; }
		else if (!stricmp(what, "well"))
		{
			// the held magazine's centre onto the gun's magazine centre: the socket moved by (well - held centre)
			DVector3 well, held, sock, f, r, u;
			if (!GunPoint(kMagAt, well) || !Alive(S.heldMag) || !Model_ActorPointToWorld(S.heldMag, DVector3(0, 0, 0), held) || !VRHandPose_TrackedMapFrame(VR_OFFHAND, sock, f, r, u))
			{ Printf("roth_colttest: no gun or no magazine in the off hand\n"); return; }
			want = sock + (well - held);
		}
		else if (!stricmp(what, "slide")) { if (!GunPoint(kSerrations, want)) { Printf("roth_colttest: no gun drawn\n"); return; } }
		else if (!stricmp(what, "support"))
		{
			// R9: the off hand's TRACKED frame onto the place support_fit measured on the main hand -- turned first (the
			// device's path probed), then its grip socket moved there
			if (!Alive(S.gun)) { Printf("roth_colttest: no gun drawn\n"); return; }
			const double err = TurnOffToSeat(pmo);
			double mainF[16], wantF[16];
			if (err < 0 || !FrameOf(VR_MAINHAND, mainF)) { Printf("roth_colttest offhand support: no hand pose (roth_xrpose hand ... first)\n"); return; }
			Mul4d(mainF, kSupportSeat[SupportSide()], wantF);
			float wf[16];
			for (int i = 0; i < 16; i++) wf[i] = (float)wantF[i];
			VSMatrix wm;
			wm.loadMatrix(wf);
			DVector3 o, ff, rr, uu;
			if (!VRPose_HandMapFrame(wm, VRHandPose_SideForRole(VR_OFFHAND) == VRHandPose::SIDE_RIGHT, o, ff, rr, uu)) { Printf("roth_colttest offhand support: no map frame\n"); return; }
			want = o;
			Printf("roth_colttest: the off hand turned to the support seat's orientation (%.2f deg left)\n", err);
		}
		else if (!stricmp(what, "pouch"))
		{
			const double yaw = pmo->Angles.Yaw.Radians(), upm = UPM();
			const double side = VRHandPose_SideForRole(VR_OFFHAND) == VRHandPose::SIDE_LEFT ? 1.0 : -1.0;
			const DVector3 head(pmo->X(), pmo->Y(), pmo->player != nullptr ? pmo->player->viewz : pmo->Z());
			want = head + DVector3(cos(yaw), sin(yaw), 0) * (kPouchF * upm) + DVector3(-sin(yaw), cos(yaw), 0) * (side * kPouchSide * upm) + DVector3(0, 0, kPouchU * upm / PS());
		}
		else { Printf("roth_colttest offhand <magbutton|pouch|well|slide|support>\n"); return; }
		if (argv.argc() >= 6) want += DVector3(atof(argv[3]), atof(argv[4]), atof(argv[5]));
		MoveOff(want, tip, what);
		return;
	}
	// HOLD THE GUN UPRIGHT: the main hand pointing (roth_xrpose point) at x y z (metres from the head) along `yaw`, its
	// wrist rolled (every 5 degrees) to the roll whose drawn gun stands most upright (its top toward world up); logs the
	// drawn bore and top. The flat test's stand-in for a player holding a pistol level.
	if (!stricmp(argv[1], "hold") && argv.argc() >= 6)
	{
		const double x = atof(argv[2]), y = atof(argv[3]), z = atof(argv[4]), yaw = atof(argv[5]);
		const double pitch = argv.argc() >= 7 ? atof(argv[6]) : 0.0;
		auto set = [&](double roll) { C_DoCommand(FStringf("roth_xrpose point main %.3f %.3f %.3f %.1f %.1f %.1f 1 0 1 0", x, y, z, yaw, pitch, roll).GetChars()); };
		double best = -2, bestRoll = 0;
		for (int r = -180; r < 180; r += 5)
		{
			set(r);
			DVector3 o, up;
			if (!GunPoint(DVector3(0, 0, 0), o) || !GunPoint(DVector3(0, 100, 0), up)) { Printf("roth_colttest hold: no gun drawn\n"); return; }
			const DVector3 d = up - o;
			const double c = d.Z / (d.Length() > 1e-9 ? d.Length() : 1);
			if (c > best) { best = c; bestRoll = r; }
		}
		set(bestRoll);
		DVector3 o, up, muz, rear, right;
		GunPoint(DVector3(0, 0, 0), o); GunPoint(DVector3(0, 100, 0), up); GunPoint(DVector3(100, 0, 0), right);
		GunPoint(kMuzzle, muz); GunPoint(kBoreRear, rear);
		const DVector3 u = (up - o).Unit(), rt = (right - o).Unit(), b = (muz - rear).Unit();
		Printf("roth_colttest hold: wrist roll %.0f: the drawn gun's top (%.2f %.2f %.2f), right side (%.2f %.2f %.2f), bore (%.2f %.2f %.2f) = yaw %.1f pitch %.1f\n",
			bestRoll, u.X, u.Y, u.Z, rt.X, rt.Y, rt.Z, b.X, b.Y, b.Z, atan2(b.Y, b.X) * 180 / M_PI, atan2(b.Z, sqrt(b.X * b.X + b.Y * b.Y)) * 180 / M_PI);
		return;
	}
	// LOOK AT A LOOSE PART: stand 146 units from the first landed part of the class (on the side toward the player) and
	// take rothdiff_shot looking at it from above (the original's steepest pitch, -126: about 45 degrees down from an
	// eye 144 units up, so the part is mid-frame). Realms and map coordinates are taken as equal here (no sector shift
	// in the study's corridor; the log shows both).
	if (!stricmp(argv[1], "lookat") && argv.argc() >= 4)
	{
		// [distance]: map units from the part (default 146: the original's steepest pitch, -126, then centres it; nearer, it
		// sits low in the frame but larger -- the original's frame is 320 x 200)
		const double away = argv.argc() >= 5 ? atof(argv[4]) : 146.0;
		PClassActor *pc = PClass::FindActor(argv[2]);
		if (pc == nullptr || S.level == nullptr) { Printf("roth_colttest lookat: no class %s\n", argv[2]); return; }
		auto it = S.level->GetThinkerIterator<AActor>();
		AActor *a, *found = nullptr;
		while ((a = it.Next()) != nullptr) if (a->IsKindOf(pc) && a->BoolVar(FName("Landed"))) { found = a; break; }
		if (found == nullptr) { Printf("roth_colttest lookat: no landed %s\n", argv[2]); return; }
		// of eight directions, the first whose camera spot and the way to it stand on the part's own floor, under a
		// ceiling above the eye (the same room, nothing between): toward the player first
		DVector2 d0 = pmo->Pos().XY() - found->Pos().XY();
		if (d0.Length() < 1e-3) d0 = DVector2(1, 0);
		d0 = d0.Unit();
		DVector2 d = d0;
		bool ok = false;
		for (int k = 0; k < 8 && !ok; k++)
		{
			const double t = k * M_PI / 4;
			d = DVector2(d0.X * cos(t) - d0.Y * sin(t), d0.X * sin(t) + d0.Y * cos(t));
			ok = true;
			for (int q = 1; q <= 6 && ok; q++)
			{
				const DVector2 p = found->Pos().XY() + d * (away * q / 6);
				sector_t *sec = S.level->PointInSector(p);
				if (sec == nullptr) { ok = false; break; }
				const double fz = sec->floorplane.ZatPoint(p), cz = sec->ceilingplane.ZatPoint(p);
				if (fabs(fz - found->floorz) > 1.0 || cz - fz < 150.0) ok = false;
			}
			// AND NO WALL BETWEEN. PointInSector answers for a spot outside the map too (the nearest subsector), so the
			// samples alone passed a camera standing in the void behind the corridor's end wall (zzzz23: a black frame
			// from (976 3845)). Every line crossed on the way out, and a player's radius past the camera, must be
			// two-sided with an opening on the part's floor tall enough for the eye.
			if (ok)
			{
				const DVector2 from = found->Pos().XY(), to = from + d * (away + 24.0);
				FPathTraverse trav(S.level, from.X, from.Y, to.X, to.Y, PT_ADDLINES);
				while (intercept_t *in = trav.Next())
				{
					if (!in->isaline) continue;
					const line_t *ln = in->d.line;
					if (ln->backsector == nullptr) { ok = false; break; }
					FLineOpening op;
					P_LineOpening(op, nullptr, ln, trav.InterceptPoint(in));
					if (op.top - op.bottom < 150.0 || fabs(op.bottom - found->floorz) > 1.0) { ok = false; break; }
				}
			}
			if (!ok) Printf("roth_colttest lookat: not from yaw %.0f (a wall, a step or a low ceiling on the way)\n", atan2(-d.Y, -d.X) * 180.0 / M_PI);
		}
		if (!ok) { d = d0; Printf("roth_colttest lookat: no clear spot that far from it on its floor; looking from toward the player\n"); }
		const DVector2 at = found->Pos().XY() + d * away;
		const double yaw = atan2(-d.Y, -d.X) * 180.0 / M_PI;
		const int ang = (int)lround((yaw - 90.0) * 512.0 / 360.0) & 511;
		Printf("roth_colttest lookat: %s landed at (%.1f %.1f %.1f); the camera at (%.0f %.0f) angle %d (yaw %.1f), pitch -126\n",
			argv[2], found->X(), found->Y(), found->Z(), at.X, at.Y, ang, yaw);
		C_DoCommand(FStringf("rothdiff_shot %d %d %d %s -126 - 20", (int)lround(at.X), (int)lround(at.Y), ang, argv[3]).GetChars());
		return;
	}
	if (!stricmp(argv[1], "rack"))
	{
		DVector3 muz, rear, sock, f, r, u;
		if (!GunPoint(kMuzzle, muz) || !GunPoint(kBoreRear, rear) || !VRHandPose_TrackedMapFrame(VR_OFFHAND, sock, f, r, u)) { Printf("roth_colttest: no gun or no off hand\n"); return; }
		const DVector3 back = (rear - muz).Unit();
		const double m = atof(argv[2]);
		// metres along the bore -> map units (the vertical through the pixel stretch)
		const DVector3 step(back.X * m * UPM(), back.Y * m * UPM(), back.Z * m * UPM() / PS());
		MoveOff(sock + step, false, FStringf("the slide pulled %.3f m", m).GetChars());
		return;
	}
	if (!stricmp(argv[1], "off") && argv.argc() >= 4)
	{
		FVRPoseTestHand h;
		if (!VRPoseTest_Hand(VR_OFFHAND, h)) { Printf("roth_colttest: no off hand\n"); return; }
		const float v = (float)atof(argv[3]);
		if (!stricmp(argv[2], "grip")) h.grip = v;
		else if (!stricmp(argv[2], "trigger")) { h.trigger = v; h.triggerTouch = v > 0; }
		VRPoseTest_SetHand(VR_OFFHAND, h);
		Printf("roth_colttest: the off hand's %s %.2f\n", argv[2], v);
		return;
	}
	Printf("roth_colttest: unknown \"%s\"\n", argv[1]);
}
