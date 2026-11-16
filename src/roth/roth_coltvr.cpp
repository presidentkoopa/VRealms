//
// roth_coltvr.cpp -- the Colt as a real M1911 in VR, in the engine. roth_coltvr.h says what each part does; the rules
// are roth_colt.h (engine-free, tools/colt_test).
//
// THE FIT (owner's plan 2026-11-16; tools/colt_test/colt_fit.cpp, its output tools/colt_test/colt_fit.txt): the hand
// stays the authored grip_pistol; the gun is placed in the hand frame by three measured points -- the beavertail on the
// thumb-index web, the index pad toward the trigger face, the grip in the wrapped fingers (no finger in the gun) -- by
// least squares; the index on the trigger is the in-between of the authored open frame and the grip's own index whose
// pad meets the face. The residuals are in the report (not all within the owner's tolerances: written down there).
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
#include "r_data/model_reach.h"
#include "model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace roth
{

namespace
{

using namespace colt;

// ---- the fit (colt_fit.txt, 2026-11-16) ----
// The seat: the gun's render coords (file x, z, y) -> the hand frame (X fingers, Y back of the hand, Z thumb axis) in
// the gun's file units, column-major; MODELDEF Scale (metres a unit) then takes it to metres (models.cpp, the grip path).
constexpr float kSeat[2][16] =
{
	{ 0.0300216867f, 0.995446897f, 0.0904664241f, 0.f, 0.0939496891f, 0.0872963214f, -0.991742309f, 0.f,
	  0.99512419f, -0.0382730693f, 0.0909011437f, 0.f, 236.13224f, 157.639087f, -63.8903402f, 1.f },     // right (Hand_Low)
	{ -0.0867959872f, -0.99242879f, 0.0868996795f, 0.f, 0.0806646454f, 0.0799413559f, 0.993530369f, 0.f,
	  0.99295502f, -0.093244181f, -0.0731153274f, 0.f, 235.654805f, 144.787435f, 66.9781703f, 1.f },     // left (Hand_Low1)
};
constexpr float kIndexReady[2] = { 0.4813f, 0.5250f };   // the index's in-between (open -> grip_pistol) on the trigger face
constexpr float kIndexFire[2] = { 0.5013f, 0.5500f };    // ... and with the trigger back its travel (10 units)
constexpr double kUnit = 0.000271900171;                 // metres a gun file unit (0.216 m muzzle to tang)

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

void Pulse(int role, double amp, int tics, const char *why)
{
	const VRMode *vm = VRMode::GetVRModeCached(true);
	const bool real = !VRPoseTest_Active() && vm != nullptr && vm->IsVR();
	if (real) VR_ScriptHaptic(role, amp, tics * 1000.0 / TICRATE);
	else Printf("[roth_colt] %s hand haptic %.2f for %d tics (%s; no headset: not sent)\n", role == VR_MAINHAND ? "main" : "off", amp, tics, why);
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
	if (S.level != level) { const double keep[6] = { S.tune[0], S.tune[1], S.tune[2], S.tune[3], S.tune[4], S.tune[5] }; S = State(); memcpy(S.tune, keep, sizeof(keep)); S.level = level; }
	AActor *pmo = Pawn();
	Hands h;
	h.vr = HandsOn(pmo) && pmo->health > 0;
	h.reloadInput = S.cmdRelease;
	h.slideRelease = S.cmdSlide;
	S.cmdRelease = S.cmdSlide = false;
	if (!h.vr || !live || !Alive(S.gun)) { const bool ri = h.reloadInput, sr = h.slideRelease; h = Hands(); h.vr = HandsOn(pmo); h.reloadInput = ri; h.slideRelease = sr; S.hands = h; return; }

	VRPickHand off;
	const bool haveOff = VRPickHandNow(VR_OFFHAND, off);
	DVector3 sock, f, r, u;
	const bool haveSock = VRHandPose_MapFrame(VR_OFFHAND, sock, f, r, u);
	h.offGrip = haveOff && off.gripClosed;
	const bool trig = haveOff && off.trigDown;
	DVector3 p;
	if (haveOff && off.havePoints && GunPoint(kMagButton, p)) h.offAtMagButton = Metres(off.tip - p) < kAtButton;
	h.magButtonPress = trig && !S.trigWas;
	S.trigWas = trig;
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
	S.hands.reloadInput = S.hands.slideRelease = false;
	if (g.shotLeft && S.hands.vr) S.spawnCase = true;
	if (g.shotLeft && S.hands.vr && g.qty == 0) Printf("[roth_colt] the last round fired: the slide locks back at the cycle's end (the word 0, i51 carried %d)\n", g.mags);
	if (e.act == ACT_NONE) return;
	S.lastAct = e.act;
	Printf("[roth_colt] %s -- %s (the word %d, i51 carried %d, lock %d)\n", ActName(e.act), e.why, g.qty, g.mags, g.lock);
	switch (e.act)
	{
	case ACT_DROP_MAG: S.spawnMag = true; Pulse(VR_MAINHAND, 0.3, 3, "magazine out"); break;
	case ACT_REFUSE_DROP: case ACT_REFUSE_TAKE: Pulse(VR_OFFHAND, 0.3, 3, "refused"); break;
	case ACT_TAKE_MAG: Pulse(VR_OFFHAND, 0.4, 3, "magazine taken"); break;
	case ACT_SEAT: if (g.qty == 0) S.reloadDue = true; Pulse(VR_MAINHAND, 0.6, 4, "seated"); Pulse(VR_OFFHAND, 0.6, 4, "seated"); break;
	case ACT_RACK_BACK: Pulse(VR_OFFHAND, 0.5, 3, "slide back"); break;
	case ACT_SLIDE_FORWARD: Pulse(VR_MAINHAND, 0.5, 3, "slide forward"); break;
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

bool ColtHeldIndex(int side, float &ready, float &fire)
{
	if (!Alive(S.gun) || !S.hands.vr) return false;
	const int k = side == VRHandPose::SIDE_LEFT ? 1 : 0;
	ready = kIndexReady[k];
	fire = kIndexFire[k];
	return true;
}

bool ColtGripTaken(int role)
{
	if (role != VR_OFFHAND || !S.hands.vr || !Alive(S.gun)) return false;
	return S.rig.magInHand || S.rig.racking || S.hands.offAtPouch || S.hands.offAtSlide || S.hands.offAtMagButton;
}

void ColtEndLevel()
{
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
//   roth_colttest offhand <magbutton|pouch|well|slide> [dx dy dz]
//                     move the off hand (its orientation kept) so that its index tip (magbutton) or its grip socket
//                     (the rest) lands on that point of the drawn gun (the pouch: of the body), plus map units
//   roth_colttest rack <metres>       move the off hand back along the drawn bore by that much (+ back, - forward)
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
		return VRHandPose_MapFrame(VR_OFFHAND, out, f, r, u);
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
			if (!GunPoint(kMagAt, well) || !Alive(S.heldMag) || !Model_ActorPointToWorld(S.heldMag, DVector3(0, 0, 0), held) || !VRHandPose_MapFrame(VR_OFFHAND, sock, f, r, u))
			{ Printf("roth_colttest: no gun or no magazine in the off hand\n"); return; }
			want = sock + (well - held);
		}
		else if (!stricmp(what, "slide")) { if (!GunPoint(kSerrations, want)) { Printf("roth_colttest: no gun drawn\n"); return; } }
		else if (!stricmp(what, "pouch"))
		{
			const double yaw = pmo->Angles.Yaw.Radians(), upm = UPM();
			const double side = VRHandPose_SideForRole(VR_OFFHAND) == VRHandPose::SIDE_LEFT ? 1.0 : -1.0;
			const DVector3 head(pmo->X(), pmo->Y(), pmo->player != nullptr ? pmo->player->viewz : pmo->Z());
			want = head + DVector3(cos(yaw), sin(yaw), 0) * (kPouchF * upm) + DVector3(-sin(yaw), cos(yaw), 0) * (side * kPouchSide * upm) + DVector3(0, 0, kPouchU * upm / PS());
		}
		else { Printf("roth_colttest offhand <magbutton|pouch|well|slide>\n"); return; }
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
		PClassActor *pc = PClass::FindActor(argv[2]);
		if (pc == nullptr || S.level == nullptr) { Printf("roth_colttest lookat: no class %s\n", argv[2]); return; }
		auto it = S.level->GetThinkerIterator<AActor>();
		AActor *a, *found = nullptr;
		while ((a = it.Next()) != nullptr) if (a->IsKindOf(pc) && a->BoolVar(FName("Landed"))) { found = a; break; }
		if (found == nullptr) { Printf("roth_colttest lookat: no landed %s\n", argv[2]); return; }
		DVector2 d = pmo->Pos().XY() - found->Pos().XY();
		if (d.Length() < 1e-3) d = DVector2(1, 0);
		d = d.Unit();
		const DVector2 at = found->Pos().XY() + d * 146.0;
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
		if (!GunPoint(kMuzzle, muz) || !GunPoint(kBoreRear, rear) || !VRHandPose_MapFrame(VR_OFFHAND, sock, f, r, u)) { Printf("roth_colttest: no gun or no off hand\n"); return; }
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
