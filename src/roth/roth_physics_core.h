#pragma once
//
// roth_physics_core -- VRealms' rigid-body world, engine-free (docs/VR_PHYSICS.md).
//
// A thin wrapper of Jolt Physics (libraries/JoltPhysics, MIT): the level as static triangles, the player's two hands
// as bodies that chase the controllers and are stopped by what is solid, and loose items as bodies that fall, tumble,
// rest and are thrown. Nothing of Jolt shows through this header, so the engine glue (roth_physics.cpp) and the
// engine-free test (tools/physics_test) see plain numbers.
//
// UNITS: metres, kilograms, seconds, in the MAP's axes (x east, y north, z up). The glue converts map units to metres
// with the VR scale (VRPickUnitsPerMetre; the Realms maps' pixel stretch is 1.0, vrealms_iwad/MAPINFO).
//
// A HAND'S FRAME is the hand frame every reader uses (r_data/vr_handpose.h VRHandPose_MapFrame): origin at the grip
// socket, X along the hand (wrist to knuckles), Y the back of the hand (the right hand's `right`, the left hand's
// `-right`), Z = X x Y (the right thumb's side is -Z, the left's +Z). A hand's pose is that frame in the map.
//
// THE HAND (after Godot XR Tools' collision hand, MIT, addons/godot-xr-tools/hands/collision_hand.gd, and
// MinimalHandPhysics, MIT, ProxyHand.cs / SlaveBone.cs: a body that follows the tracked hand): a dynamic body without
// gravity whose velocity each step is what would carry it to the tracked pose in that step, capped. Free, it is where
// the hand is; against a wall or a desk it stops there and the drawn hand with it (the glue's correction); it pushes
// loose items with its own mass. Further than TeleportM from its target (the player walked through a door, or the
// hand went into a wall and out the other side), it is put back on the target, as collision_hand.gd does at 1 m.
//
// WHAT A HAND HOLDS is part of the hand body (a compound of the palm and the held item's hull), so the held item
// collides with the world as the hand does and never jitters on a joint.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//
#include <cstdint>
#include <memory>
#include <vector>

namespace roth
{
namespace phys
{

struct V3
{
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	V3 operator+(const V3 &o) const { return V3(x + o.x, y + o.y, z + o.z); }
	V3 operator-(const V3 &o) const { return V3(x - o.x, y - o.y, z - o.z); }
	V3 operator*(double s) const { return V3(x * s, y * s, z * s); }
	double Dot(const V3 &o) const { return x * o.x + y * o.y + z * o.z; }
	V3 Cross(const V3 &o) const { return V3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x); }
	double Length() const;
};

// A rotation as a unit quaternion (x, y, z the axis times sin(angle / 2), w cos(angle / 2)).
struct Q
{
	double x = 0, y = 0, z = 0, w = 1;
	Q() = default;
	Q(double a, double b, double c, double d) : x(a), y(b), z(c), w(d) {}
	static Q FromBasis(const V3 &X, const V3 &Y, const V3 &Z);   // the columns of the rotation (orthonormal)
	void Basis(V3 &X, V3 &Y, V3 &Z) const;
	Q operator*(const Q &o) const;
	Q Conj() const { return Q(-x, -y, -z, w); }
	V3 Rotate(const V3 &v) const;
	Q Normalized() const;
};

struct Pose
{
	V3 p;
	Q q;
	V3 Apply(const V3 &local) const { return p + q.Rotate(local); }
	Pose Inverse() const { const Q c = q.Conj(); return Pose{ c.Rotate(p * -1.0), c }; }
	Pose operator*(const Pose &o) const { return Pose{ Apply(o.p), (q * o.q).Normalized() }; }
};

using Id = uint32_t;
constexpr Id kNone = 0xffffffffu;

// The palm: a box in the hand's frame (metres).
struct HandShape
{
	V3 centre = V3(0.035, 0.0, 0.0);
	V3 half = V3(0.075, 0.018, 0.045);
	double massKg = 1.5;
};

struct ItemShape
{
	std::vector<V3> hull;   // points in the body's frame, metres; fewer than 4 or flat: a box round them
	double massKg = 0.3;
	double friction = 0.6;
	double restitution = 0.15;
};

class World
{
public:
	World();
	~World();
	World(const World &) = delete;
	World &operator=(const World &) = delete;

	bool Ok() const;

	// THE LEVEL. Each key (a sector, in the glue) owns one static body of triangles (9 floats each: three corners,
	// metres, world); setting a key again replaces it, an empty list removes it.
	void SetStatic(int key, const std::vector<float> &tris);
	void ClearStatic();
	size_t StaticTriangles() const;

	// LOOSE ITEMS. `graceHand` (0 or 1, -1 none): the hand that just let go of it does not touch it for `graceSteps`
	// steps, so an item leaving the palm is not knocked by the palm it overlaps.
	Id AddItem(const ItemShape &shape, const Pose &at, const V3 &vel, const V3 &angVel, int graceHand = -1, int graceSteps = 0);
	void Remove(Id id);
	bool PoseOf(Id id, Pose &out) const;
	bool VelocityOf(Id id, V3 &lin, V3 &ang) const;
	bool Resting(Id id) const;          // asleep: it has come to rest
	// The loose item touching a ball of `radius` round `p` nearest it, kNone if none; `dist` its distance.
	Id ItemNear(const V3 &p, double radius, double *dist = nullptr) const;

	// HANDS.
	Id AddHand(const HandShape &palm, const Pose &at);
	// What the hand holds: `shape`'s hull at `rel` in the hand's frame (nullptr: nothing).
	void HandHolds(Id hand, const ItemShape *shape, const Pose &rel);
	// Chase `target` this step (dt seconds); put it there if it is further than TeleportM.
	void DriveHand(Id hand, const Pose &target, double dt);
	void TeleportHand(Id hand, const Pose &target);
	bool HandPose(Id hand, Pose &out) const;
	// A GHOST HAND meets nothing (it slips through what held it: roth_physics.cpp lets a hand stuck a moment go);
	// HandFitsAt: the hand's shape at `at` overlaps nothing of the level (where a ghost can be solid again).
	void SetHandGhost(Id hand, bool ghost);
	bool HandGhost(Id hand) const;
	bool HandFitsAt(Id hand, const Pose &at) const;
	// Something of the level between two points (a ray against the static triangles).
	bool StaticBetween(const V3 &a, const V3 &b) const;
	// The hardest hit the hand (or what it holds) took in the last step: the closing speed along the contact's normal,
	// m/s (0: none); and whether it touches anything now.
	double HandHit(Id hand) const;
	bool HandTouching(Id hand) const;

	void Step(double dt, int collisionSteps = 2);

	size_t Bodies() const;

	static constexpr double TeleportM = 0.6;          // further from its controller than this, a hand is put back on it ...
	static constexpr double TeleportFarM = 1.5;       // ... unless the level is between them; this far, it is put back anyway
	static constexpr double MaxHandSpeed = 12.0;      // m/s
	static constexpr double MaxHandSpin = 40.0;       // rad/s
	static constexpr double KnockSpeed = 2.5;         // m/s: a hand slower than this passes into loose items (a reach)

private:
	struct Impl;
	std::unique_ptr<Impl> I;
};

// The Euler angles a GZDoom model takes for a rotation in the MAP's axes (r_data/models.cpp: the model turns by
// Ry(-yaw) Rz(pitch) Rx(-roll) in its render axes, which are the map's with y and z swapped): R = Rz(yaw) Ry(-pitch)
// Rx(roll) in the map. Degrees.
void ModelAngles(const Q &q, double &yaw, double &pitch, double &roll);

} // namespace phys
} // namespace roth
