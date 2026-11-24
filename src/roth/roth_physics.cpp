//
// roth_physics -- see the header and docs/VR_PHYSICS.md.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//
#include "roth_physics.h"
#include "roth_physics_core.h"
#include "roth_img3d.h"
#include "roth_vrpick.h"
#include "roth_interface.h"
#include "roth_inventory.h"
#include "roth_held.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"

#include <math.h>
#include <algorithm>
#include <memory>
#include <vector>

CVAR(Bool, roth_physics, true, 0)          // physical hands and loose items; not saved
CVAR(Bool, roth_physics_debug, false, 0)   // a line when a hand is stopped or freed, and a loose item's rest

namespace roth
{

namespace
{

using phys::Pose;
using phys::Q;
using phys::V3;

constexpr double kDt = 1.0 / 35.0;            // a game tic
constexpr double kBlockedM = 0.004;           // further than this from its controller, the hand is drawn where it stopped
constexpr double kBlockedDeg = 2.0;
constexpr double kHitFelt = 0.35;             // m/s: a hit the hand feels
constexpr double kGrabRadiusM = 0.08;         // the palm reaches a loose item this far from its surface
constexpr double kReturnM = 10.0;             // a loose item this far from the player goes back into the pack
const V3 kPalmPoint(0.045, 0.0, 0.0);         // where the palm grabs, in the hand frame

struct Loose
{
	int item = 0;
	phys::Id body = phys::kNone;
	double metres = 0;
	int slot = -1;
	bool restLogged = false;
};

struct HandRec
{
	phys::Id body = phys::kNone;
	int heldItem = 0;          // what its body holds
	Pose rel;                  // the held item's body frame in the hand frame
	int pendingItem = 0;       // a loose item just taken: when the game puts it in this hand, it sits where it was
	Pose pendingRel;
	Pose hist[4];
	int histN = 0;
	bool blocked = false;
	bool haveTarget = false;
};

struct State
{
	FLevelLocals *level = nullptr;
	int levelStamp = -1;
	std::unique_ptr<phys::World> world;
	std::vector<double> floorD, ceilD;
	std::vector<std::vector<int>> subsOf;   // the subsectors of each sector
	std::vector<std::vector<int>> nbrs;     // the sectors across each sector's two-sided lines
	HandRec hand[2];
	std::vector<Loose> loose;
	double upm = 164.3;
	bool loggedOff = false;
} S;

const char *RoleName(int role) { return role == VR_MAINHAND ? "main" : "off"; }
bool Debug() { return roth_physics_debug || VRPickTesting(); }

V3 M(const DVector3 &v) { return V3(v.X / S.upm, v.Y / S.upm, v.Z / S.upm); }
DVector3 U(const V3 &v) { return DVector3(v.x * S.upm, v.y * S.upm, v.z * S.upm); }

void Put(std::vector<float> &t, const DVector3 &a)
{
	t.push_back((float)(a.X / S.upm));
	t.push_back((float)(a.Y / S.upm));
	t.push_back((float)(a.Z / S.upm));
}

void Tri(std::vector<float> &t, const DVector3 &a, const DVector3 &b, const DVector3 &c) { Put(t, a); Put(t, b); Put(t, c); }

// A quad up a line: from z0 to z1 at v1, from w0 to w1 at v2, when it has any height.
void Strip(std::vector<float> &t, const DVector2 &v1, const DVector2 &v2, double z0, double z1, double w0, double w1)
{
	if (z1 - z0 < 0.01 && w1 - w0 < 0.01) return;
	const DVector3 a(v1, z0), b(v2, w0), c(v2, std::max(w0, w1)), d(v1, std::max(z0, z1));
	Tri(t, a, b, c);
	Tri(t, a, c, d);
}

// One sector's share of the level: its floors and ceilings (its GL subsectors, convex, fanned) and the solid parts of
// every line that bounds it, as they stand now.
std::vector<float> SectorTris(FLevelLocals *level, int si)
{
	std::vector<float> t;
	sector_t *sec = &level->sectors[si];
	for (int ss : S.subsOf[si])
	{
		subsector_t &sub = level->subsectors[ss];
		if (sub.numlines < 3) continue;
		std::vector<DVector2> poly;
		for (uint32_t k = 0; k < sub.numlines; k++) poly.push_back(sub.firstline[k].v1->fPos());
		for (size_t k = 1; k + 1 < poly.size(); k++)
		{
			const DVector2 &a = poly[0], &b = poly[k], &c = poly[k + 1];
			Tri(t, DVector3(a, sec->floorplane.ZatPoint(a)), DVector3(b, sec->floorplane.ZatPoint(b)), DVector3(c, sec->floorplane.ZatPoint(c)));
			Tri(t, DVector3(a, sec->ceilingplane.ZatPoint(a)), DVector3(b, sec->ceilingplane.ZatPoint(b)), DVector3(c, sec->ceilingplane.ZatPoint(c)));
		}
	}
	for (line_t *ln : sec->Lines)
	{
		if (ln == nullptr || ln->frontsector == nullptr) continue;
		const DVector2 v1 = ln->v1->fPos(), v2 = ln->v2->fPos();
		sector_t *f = ln->frontsector, *b = ln->backsector;
		const double ff1 = f->floorplane.ZatPoint(v1), ff2 = f->floorplane.ZatPoint(v2);
		const double fc1 = f->ceilingplane.ZatPoint(v1), fc2 = f->ceilingplane.ZatPoint(v2);
		if (b == nullptr)
		{
			Strip(t, v1, v2, ff1, fc1, ff2, fc2);
			continue;
		}
		const double bf1 = b->floorplane.ZatPoint(v1), bf2 = b->floorplane.ZatPoint(v2);
		const double bc1 = b->ceilingplane.ZatPoint(v1), bc2 = b->ceilingplane.ZatPoint(v2);
		// the step between the floors, and the lintel between the ceilings (a closed door: its whole height)
		Strip(t, v1, v2, std::min(ff1, bf1), std::max(ff1, bf1), std::min(ff2, bf2), std::max(ff2, bf2));
		Strip(t, v1, v2, std::min(fc1, bc1), std::max(fc1, bc1), std::min(fc2, bc2), std::max(fc2, bc2));
	}
	return t;
}

void BuildLevel(FLevelLocals *level)
{
	S.world.reset(new phys::World());
	const int ns = (int)level->sectors.Size();
	S.floorD.assign(ns, 0);
	S.ceilD.assign(ns, 0);
	S.subsOf.assign(ns, {});
	S.nbrs.assign(ns, {});
	for (unsigned i = 0; i < level->subsectors.Size(); i++)
	{
		sector_t *sec = level->subsectors[i].sector;
		if (sec != nullptr) S.subsOf[sec->Index()].push_back((int)i);
	}
	for (unsigned i = 0; i < level->lines.Size(); i++)
	{
		line_t &ln = level->lines[i];
		if (ln.frontsector == nullptr || ln.backsector == nullptr || ln.frontsector == ln.backsector) continue;
		const int a = ln.frontsector->Index(), b = ln.backsector->Index();
		if (std::find(S.nbrs[a].begin(), S.nbrs[a].end(), b) == S.nbrs[a].end()) S.nbrs[a].push_back(b);
		if (std::find(S.nbrs[b].begin(), S.nbrs[b].end(), a) == S.nbrs[b].end()) S.nbrs[b].push_back(a);
	}
	for (int si = 0; si < ns; si++)
	{
		S.world->SetStatic(si, SectorTris(level, si));
		S.floorD[si] = level->sectors[si].floorplane.fD();
		S.ceilD[si] = level->sectors[si].ceilingplane.fD();
	}
	for (int r = 0; r < 2; r++) S.hand[r] = HandRec();
	S.loose.clear();
	Printf("[roth_physics] the level is solid: %d sectors, %d triangles (%.1f units a metre)\n", ns, (int)S.world->StaticTriangles(), S.upm);
}

// Sectors that moved since the last tic (a door, a lift, a crusher) are rebuilt, with their neighbours, whose walls
// they bound.
void FollowSectors(FLevelLocals *level)
{
	std::vector<int> moved;
	for (int si = 0; si < (int)S.floorD.size(); si++)
	{
		const sector_t &sec = level->sectors[si];
		if (sec.floorplane.fD() == S.floorD[si] && sec.ceilingplane.fD() == S.ceilD[si]) continue;
		S.floorD[si] = sec.floorplane.fD();
		S.ceilD[si] = sec.ceilingplane.fD();
		moved.push_back(si);
	}
	if (moved.empty()) return;
	std::vector<int> todo = moved;
	for (int si : moved) for (int n : S.nbrs[si]) todo.push_back(n);
	std::sort(todo.begin(), todo.end());
	todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
	for (int si : todo) S.world->SetStatic(si, SectorTris(level, si));
}

// The tracked hand frame as a pose in metres (roth_physics_core.h: X along the hand, Y the back of the hand).
bool TrackedPose(int role, Pose &out)
{
	DVector3 o, f, r, u;
	if (!VRHandPose_TrackedMapFrame(role, o, f, r, u)) return false;
	const bool right = VRHandPose_SideForRole(role) == VRHandPose::SIDE_RIGHT;
	const V3 X(f.X, f.Y, f.Z);
	const V3 Y = right ? V3(r.X, r.Y, r.Z) : V3(-r.X, -r.Y, -r.Z);
	const V3 Z = X.Cross(Y);
	out.p = M(o);
	out.q = Q::FromBasis(X, Y, Z);
	return true;
}

double AngleDeg(const Q &a, const Q &b)
{
	const double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	return 2.0 * acos(std::min(1.0, d)) * 57.29577951308232;
}

// The world's move that takes the tracked frame onto the physics hand: render axes (the map's with y and z swapped),
// map units, column-major.
void BlockedMatrix(const Pose &phys, const Pose &tracked, float out[16])
{
	const Q R = (phys.q * tracked.q.Conj()).Normalized();
	const V3 t = phys.p - R.Rotate(tracked.p);
	V3 cols[3];
	R.Basis(cols[0], cols[1], cols[2]);
	const double Rm[3][3] = { { cols[0].x, cols[1].x, cols[2].x }, { cols[0].y, cols[1].y, cols[2].y }, { cols[0].z, cols[1].z, cols[2].z } };
	static const int sw[3] = { 0, 2, 1 };
	for (int i = 0; i < 16; i++) out[i] = 0.f;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) out[c * 4 + r] = (float)Rm[sw[r]][sw[c]];
	const double tm[3] = { t.x * S.upm, t.y * S.upm, t.z * S.upm };
	for (int r = 0; r < 3; r++) out[12 + r] = (float)tm[sw[r]];
	out[15] = 1.f;
}

Pose FromRel12(const double *rel)
{
	const V3 X(rel[0], rel[3], rel[6]), Y(rel[1], rel[4], rel[7]), Z(rel[2], rel[5], rel[8]);
	return Pose{ V3(rel[9], rel[10], rel[11]), Q::FromBasis(X, Y, Z) };
}

void ToRel12(const Pose &p, double *rel)
{
	V3 X, Y, Z;
	p.q.Basis(X, Y, Z);
	const double m[9] = { X.x, Y.x, Z.x, X.y, Y.y, Z.y, X.z, Y.z, Z.z };
	for (int i = 0; i < 9; i++) rel[i] = m[i];
	rel[9] = p.p.x; rel[10] = p.p.y; rel[11] = p.p.z;
}

double ItemMetres(int item) { return Img3dHandMetres(item, HeldCardMetres(item)); }

bool ItemShapeOf(int item, double metres, phys::ItemShape &out)
{
	std::vector<DVector3> hull;
	if (!Img3dItemHull(item, metres, hull) || hull.empty()) return false;
	out.hull.clear();
	V3 lo(1e9, 1e9, 1e9), hi(-1e9, -1e9, -1e9);
	for (const DVector3 &p : hull)
	{
		out.hull.push_back(V3(p.X, p.Y, p.Z));
		lo = V3(std::min(lo.x, p.X), std::min(lo.y, p.Y), std::min(lo.z, p.Z));
		hi = V3(std::max(hi.x, p.X), std::max(hi.y, p.Y), std::max(hi.z, p.Z));
	}
	// its weight from its size: a third of its box at 1.2 kg a litre, a gram at least [R: a tuning rule]
	const V3 e = hi - lo;
	out.massKg = std::max(0.02, std::min(3.0, e.x * e.y * e.z * 1200.0 / 3.0));
	return true;
}

// The hand's own velocity over the last three tics: what a let go throws with (Godot XR Tools' function_pickup.gd
// averages the last few physics frames the same way).
void HandVelocity(const HandRec &h, V3 &lin, V3 &ang)
{
	lin = V3();
	ang = V3();
	const int n = std::min(h.histN, 4);
	if (n < 2) return;
	const Pose &now = h.hist[(h.histN - 1) % 4], &then = h.hist[(h.histN - n) % 4];
	const double T = (n - 1) * kDt;
	lin = (now.p - then.p) * (1.0 / T);
	Q d = (now.q * then.q.Conj()).Normalized();
	if (d.w < 0) d = Q(-d.x, -d.y, -d.z, -d.w);
	const double s = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (s > 1e-9) ang = V3(d.x / s, d.y / s, d.z / s) * (2.0 * atan2(s, d.w) / T);
}

int FreeSlot()
{
	for (int s = 0; s < kImg3dLoose; s++)
	{
		bool used = false;
		for (const Loose &l : S.loose) if (l.slot == s) used = true;
		if (!used) return s;
	}
	return -1;
}

void RemoveLoose(size_t i, const char *why)
{
	if (i >= S.loose.size()) return;
	Printf("[roth_physics] loose item %d: %s\n", S.loose[i].item, why);
	if (S.world) S.world->Remove(S.loose[i].body);
	S.loose.erase(S.loose.begin() + i);
}

// What each hand's body holds follows what the game has in that hand.
void FollowHeld(int role)
{
	HandRec &h = S.hand[role];
	if (h.body == phys::kNone) return;
	const int item = VRPickHeldItem(role);
	if (item == h.heldItem) return;
	h.heldItem = 0;
	S.world->HandHolds(h.body, nullptr, Pose());
	Img3dHeldRel(role, nullptr);
	if (item <= 0 || !Img3dHas(item)) { h.heldItem = item; h.pendingItem = 0; return; }
	const double metres = ItemMetres(item);
	phys::ItemShape shape;
	if (!ItemShapeOf(item, metres, shape)) { h.heldItem = item; return; }
	Pose rel;
	if (h.pendingItem == item)
	{
		rel = h.pendingRel;   // taken where it lay: kept there
		double r12[12];
		ToRel12(rel, r12);
		Img3dHeldRel(role, r12);
	}
	else
	{
		double r12[12];
		if (!Img3dGripRel(item, VRHandPose_SideForRole(role) == VRHandPose::SIDE_RIGHT, metres, r12)) { h.heldItem = item; return; }
		rel = FromRel12(r12);
	}
	h.pendingItem = 0;
	S.world->HandHolds(h.body, &shape, rel);
	h.heldItem = item;
	h.rel = rel;
	if (Debug()) Printf("[roth_physics] %s hand: item %d is part of its body now (%.2f m, %d points)\n", RoleName(role), item, metres, (int)shape.hull.size());
}

void TickHands(FLevelLocals *level, bool world)
{
	for (int role = 0; role < 2; role++)
	{
		HandRec &h = S.hand[role];
		Pose target;
		h.haveTarget = TrackedPose(role, target);
		if (!h.haveTarget || !world)
		{
			if (h.blocked) { VRHandPose_SetBlocked(role, nullptr); h.blocked = false; }
			continue;
		}
		if (h.body == phys::kNone)
		{
			phys::HandShape palm;
			h.body = S.world->AddHand(palm, target);
			Printf("[roth_physics] the %s hand is a body now: it stops at what is solid\n", RoleName(role));
		}
		FollowHeld(role);
		S.world->DriveHand(h.body, target, kDt);
	}
}

void AfterStep()
{
	for (int role = 0; role < 2; role++)
	{
		HandRec &h = S.hand[role];
		if (h.body == phys::kNone || !h.haveTarget) continue;
		Pose target, now;
		if (!TrackedPose(role, target) || !S.world->HandPose(h.body, now)) continue;
		h.hist[h.histN % 4] = now;
		h.histN++;
		const double off = (now.p - target.p).Length(), turn = AngleDeg(now.q, target.q);
		const bool blocked = off > kBlockedM || turn > kBlockedDeg;
		if (blocked)
		{
			float m[16];
			BlockedMatrix(now, target, m);
			VRHandPose_SetBlocked(role, m);
		}
		else if (h.blocked) VRHandPose_SetBlocked(role, nullptr);
		if (blocked != h.blocked && Debug())
			Printf("[roth_physics] %s hand %s (%.1f cm, %.1f deg from its controller)\n", RoleName(role),
				blocked ? "STOPPED by something solid: drawn where it stopped" : "free again", off * 100.0, turn);
		h.blocked = blocked;
		const double hit = S.world->HandHit(h.body);
		if (hit > kHitFelt)
		{
			const double amp = std::min(1.0, 0.25 + hit * 0.35);
			VRPickHaptic(role, amp, 25, Debug() ? "the hand hit something" : nullptr);
			if (Debug()) Printf("[roth_physics] %s hand hits at %.2f m/s: a %.2f buzz\n", RoleName(role), hit, amp);
		}
	}
}

void TickLoose(FLevelLocals *level)
{
	Inventory *inv = InterfaceInventory();
	AActor *pmo = level->GetConsolePlayer() != nullptr ? level->GetConsolePlayer()->mo : nullptr;
	for (size_t i = 0; i < S.loose.size();)
	{
		Loose &l = S.loose[i];
		const int slot = inv != nullptr ? inv->FindSlotHolding(l.item) : -1;
		if (slot < 0) { RemoveLoose(i, "the game no longer carries it (used or given): gone"); continue; }
		if (inv->RightHand() == slot && VRPickHeldItem(0) != l.item && VRPickHeldItem(1) != l.item && S.hand[0].pendingItem != l.item && S.hand[1].pendingItem != l.item)
		{
			RemoveLoose(i, "selected from the inventory: it is in the hand now, not on the floor");
			continue;
		}
		Pose p;
		if (!S.world->PoseOf(l.body, p)) { RemoveLoose(i, "its body is gone"); continue; }
		const DVector3 at = U(p.p);
		if (pmo != nullptr && (at - pmo->Pos()).Length() > kReturnM * S.upm) { RemoveLoose(i, "left behind: back in the pack"); continue; }
		if (p.p.z * S.upm < -32768.0) { RemoveLoose(i, "fell out of the level: back in the pack"); continue; }
		double yaw, pitch, roll;
		phys::ModelAngles(p.q, yaw, pitch, roll);
		Img3dLooseShow(level, l.slot, l.item, at, yaw, pitch, roll, l.metres);
		if (!l.restLogged && S.world->Resting(l.body))
		{
			l.restLogged = true;
			Printf("[roth_physics] loose item %d comes to rest at (%.1f %.1f %.1f), turned yaw %.0f pitch %.0f roll %.0f\n", l.item, at.X, at.Y, at.Z, yaw, pitch, roll);
		}
		i++;
	}
}

} // namespace

bool PhysicsOn() { return roth_physics && S.world != nullptr; }

void PhysicsTick(FLevelLocals *level, bool world)
{
	if (level == nullptr) return;
	if (!roth_physics)
	{
		if (!S.loggedOff) { Printf("[roth_physics] off (roth_physics 0): the hands go where the controllers go\n"); S.loggedOff = true; }
		for (int r = 0; r < 2; r++) VRHandPose_SetBlocked(r, nullptr);
		return;
	}
	S.loggedOff = false;
	S.upm = VRPickUnitsPerMetre();
	if (S.level != level || S.world == nullptr || level->maptime < S.levelStamp)
	{
		S.level = level;
		BuildLevel(level);
	}
	S.levelStamp = level->maptime;
	FollowSectors(level);
	TickHands(level, world);
	if (world) S.world->Step(kDt, 2);
	if (world) AfterStep();
	TickLoose(level);
}

void PhysicsEndLevel()
{
	for (int r = 0; r < 2; r++) VRHandPose_SetBlocked(r, nullptr);
	S.world.reset();
	S.level = nullptr;
	S.loose.clear();
	for (int r = 0; r < 2; r++) S.hand[r] = HandRec();
}

bool PhysicsLetGo(int role, int item)
{
	if (!PhysicsOn() || role < 0 || role > 1 || item <= 0 || !Img3dHas(item)) return false;
	HandRec &h = S.hand[role];
	if (h.body == phys::kNone || h.heldItem != item) return false;
	FLevelLocals *level = S.level;
	AActor *pmo = level != nullptr && level->GetConsolePlayer() != nullptr ? level->GetConsolePlayer()->mo : nullptr;
	Pose hp;
	if (!S.world->HandPose(h.body, hp)) return false;
	// OVER THE SHOULDER (3b): behind the face, above the chest -- into the pack, as before
	if (pmo != nullptr)
	{
		const double vh = pmo->player != nullptr ? pmo->player->viewheight : 41.0;
		const DVector3 head = pmo->Pos() + DVector3(0, 0, vh);
		const DVector3 rel = U(hp.p) - head;
		const DVector3 fwd(pmo->Angles.Yaw.Cos(), pmo->Angles.Yaw.Sin(), 0);
		const double ahead = (rel | fwd) / S.upm, below = -rel.Z / S.upm, wide = DVector2(rel.X, rel.Y).Length() / S.upm;
		if (ahead < 0.10 && below < 0.35 && wide < 0.50)
		{
			if (Debug()) Printf("[roth_physics] %s hand lets go over the shoulder (%.2f m ahead of the eyes, %.2f m below): into the pack\n", RoleName(role), ahead, below);
			return false;
		}
	}
	const double metres = ItemMetres(item);
	phys::ItemShape shape;
	if (!ItemShapeOf(item, metres, shape)) return false;
	const int slot = FreeSlot();
	if (slot < 0) RemoveLoose(0, "the oldest of too many on the floor: back in the pack");
	const Pose body = hp * h.rel;
	V3 lin, ang;
	HandVelocity(h, lin, ang);
	lin = lin + ang.Cross(body.p - hp.p);   // the wrist's flick carries the far end faster
	S.world->HandHolds(h.body, nullptr, Pose());
	h.heldItem = 0;
	Img3dHeldRel(role, nullptr);
	Loose l;
	l.item = item;
	l.metres = metres;
	l.slot = slot >= 0 ? slot : FreeSlot();
	l.body = S.world->AddItem(shape, body, lin, ang);
	if (l.body == phys::kNone) return false;
	S.loose.push_back(l);
	const DVector3 at = U(body.p);
	Printf("[roth_physics] %s hand lets go of item %d: it is LOOSE in the world at (%.1f %.1f %.1f), %.2f m/s %s, %.2f kg\n", RoleName(role), item,
		at.X, at.Y, at.Z, lin.Length(), lin.Length() > 1.0 ? "(thrown)" : "(dropped)", shape.massKg);
	return true;
}

int PhysicsGrabLoose(int role)
{
	if (!PhysicsOn() || role < 0 || role > 1) return 0;
	HandRec &h = S.hand[role];
	if (h.body == phys::kNone) return 0;
	Pose hp;
	if (!S.world->HandPose(h.body, hp)) return 0;
	double d = -1;
	const phys::Id id = S.world->ItemNear(hp.Apply(kPalmPoint), kGrabRadiusM, &d);
	if (id == phys::kNone) return 0;
	for (size_t i = 0; i < S.loose.size(); i++)
	{
		if (S.loose[i].body != id) continue;
		Pose bp;
		if (!S.world->PoseOf(id, bp)) return 0;
		const int item = S.loose[i].item;
		h.pendingItem = item;
		h.pendingRel = hp.Inverse() * bp;   // where it lies, in the hand: kept
		S.world->Remove(id);
		S.loose.erase(S.loose.begin() + i);
		Printf("[roth_physics] %s hand takes loose item %d where it touched it (%.1f cm from the palm)\n", RoleName(role), item, d * 100.0);
		return item;
	}
	return 0;
}

AActor *PhysicsLooseActor(int index)
{
	if (index < 0 || index >= (int)S.loose.size()) return nullptr;
	return Img3dLooseActor(S.loose[index].slot);
}

CCMD(roth_physics_status)
{
	if (S.world == nullptr) { Printf("[roth_physics] no world (%s)\n", roth_physics ? "no level yet" : "roth_physics 0"); return; }
	Printf("[roth_physics] %d bodies, %d static triangles, %.1f units a metre\n", (int)S.world->Bodies(), (int)S.world->StaticTriangles(), S.upm);
	for (int r = 0; r < 2; r++)
	{
		const HandRec &h = S.hand[r];
		Pose p;
		if (h.body == phys::kNone || !S.world->HandPose(h.body, p)) { Printf("  %s hand: no body\n", RoleName(r)); continue; }
		const DVector3 at = U(p.p);
		Printf("  %s hand at (%.1f %.1f %.1f)%s, holding item %d\n", RoleName(r), at.X, at.Y, at.Z, h.blocked ? ", STOPPED" : "", h.heldItem);
	}
	for (const Loose &l : S.loose)
	{
		Pose p;
		if (!S.world->PoseOf(l.body, p)) continue;
		const DVector3 at = U(p.p);
		Printf("  loose item %d at (%.1f %.1f %.1f)%s\n", l.item, at.X, at.Y, at.Z, S.world->Resting(l.body) ? ", at rest" : ", moving");
	}
}

} // namespace roth
